// SPDX-License-Identifier: Apache-2.0
/*
 * ESP-Hosted SPI half-duplex/QIO firmware transport.
 *
 * The host may split a Hosted frame over several WRDMA + CMD7 transactions
 * according to negotiated transfer limits. This backend assembles
 * segments until the standard esp_payload_header describes a complete frame.
 *
 * E2H frames are queued as one SPI Slave HD TX transaction and are consumed by
 * one RDDMA + CMD8 on the host.
 */

#include "sdkconfig.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/spi_slave_hd.h"
#include "driver/spi_common.h"
#include "esp.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_fw_version.h"
#include "../../../../host/include/esp_spi_hd_wire.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/spi_types.h"
#include "soc/soc_caps.h"
#include "interface.h"
#include "stats.h"
#include "endian.h"
#include <rom/rtc.h>

#define SPI_HD_HOST                    SPI2_HOST
#define SPI_HD_DMA_CHAN                SPI_DMA_CH_AUTO

/* Verify the entire shared-register ABI against the selected silicon. */
_Static_assert(SPIHD_REGISTER_BYTES <= SOC_SPI_MAXIMUM_BUFFER_SIZE,
               "SPI-HD shared register layout exceeds hardware capacity");

#define SPI_HD_FRAME_BUF_SIZE          1600U
#define SPI_HD_RX_QUEUE_SIZE           CONFIG_ESP_SPI_HD_RX_Q_SIZE
#define SPI_HD_RX_SEGMENT_SIZE         CONFIG_ESP_SPI_HD_RX_SEGMENT_SIZE

_Static_assert((SPI_HD_RX_SEGMENT_SIZE % 4) == 0,
               "SPI-HD RX segment size must be a multiple of 4 for DMA");

#define GPIO_CS                        CONFIG_ESP_SPI_HD_GPIO_CS
#define GPIO_SCLK                      CONFIG_ESP_SPI_HD_GPIO_CLK
#define GPIO_D0                        CONFIG_ESP_SPI_HD_GPIO_D0
#define GPIO_D1                        CONFIG_ESP_SPI_HD_GPIO_D1
#define GPIO_D2                        CONFIG_ESP_SPI_HD_GPIO_D2
#define GPIO_D3                        CONFIG_ESP_SPI_HD_GPIO_D3
#define GPIO_DATA_READY                CONFIG_ESP_SPI_HD_GPIO_DATA_READY

/* GPIO matrix selection is board policy, but bus pins must be usable and distinct. */
_Static_assert(GPIO_IS_VALID_GPIO(GPIO_CS),
               "SPI-HD CS pin is invalid");
_Static_assert(GPIO_IS_VALID_GPIO(GPIO_SCLK),
               "SPI-HD SCLK pin is invalid");
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(GPIO_D0),
               "SPI-HD D0 pin is invalid");
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(GPIO_D1),
               "SPI-HD D1 pin is invalid");
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(GPIO_D2),
               "SPI-HD D2 pin is invalid");
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(GPIO_D3),
               "SPI-HD D3 pin is invalid");
_Static_assert(GPIO_IS_VALID_OUTPUT_GPIO(GPIO_DATA_READY),
               "SPI-HD DATA_READY pin is invalid");
_Static_assert(GPIO_CS != GPIO_SCLK &&
               GPIO_CS != GPIO_D0 &&
               GPIO_CS != GPIO_D1 &&
               GPIO_CS != GPIO_D2 &&
               GPIO_CS != GPIO_D3 &&
               GPIO_CS != GPIO_DATA_READY &&
               GPIO_SCLK != GPIO_D0 &&
               GPIO_SCLK != GPIO_D1 &&
               GPIO_SCLK != GPIO_D2 &&
               GPIO_SCLK != GPIO_D3 &&
               GPIO_SCLK != GPIO_DATA_READY &&
               GPIO_D0 != GPIO_D1 &&
               GPIO_D0 != GPIO_D2 &&
               GPIO_D0 != GPIO_D3 &&
               GPIO_D0 != GPIO_DATA_READY &&
               GPIO_D1 != GPIO_D2 &&
               GPIO_D1 != GPIO_D3 &&
               GPIO_D1 != GPIO_DATA_READY &&
               GPIO_D2 != GPIO_D3 &&
               GPIO_D2 != GPIO_DATA_READY &&
               GPIO_D3 != GPIO_DATA_READY,
               "SPI-HD GPIOs must be distinct");

static const char TAG[] = "FW_SPI_HD";
static uint32_t s_host_rx_max;

typedef struct {
    spi_slave_hd_data_t trans;
    uint8_t *buf;
} spi_hd_rx_slot_t;

typedef struct {
    spi_slave_hd_data_t trans;
    uint8_t *buf;
    bool raw_tp;
    uint32_t raw_tp_run_id;
    uint32_t raw_tp_frames;
} spi_hd_tx_desc_t;

#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
#define SPI_HD_TX_BATCH_FRAMES 2U
#define SPI_HD_TX_POOL_SIZE CONFIG_ESP_SPI_HD_TX_POOL_SIZE
#define SPI_HD_TX_BATCH_BUFFERS 1U
#define SPI_HD_TX_BATCH_SIZE (SPI_HD_FRAME_BUF_SIZE * SPI_HD_TX_BATCH_FRAMES)
/* Pinned IDF append_trans accepts one DMA descriptor, at most 4092 bytes,
 * even when bus max_transfer_sz allocates a larger descriptor ring. */
_Static_assert(SPI_HD_TX_BATCH_SIZE <= 4092U,
               "SPI-HD TX batch exceeds the append transaction limit");
typedef struct {
    uint8_t *buf;
    size_t len;
    bool raw_tp;
    uint32_t run_id;
} spi_hd_tx_slot_t;
static spi_hd_tx_slot_t s_tx_pool[SPI_HD_TX_POOL_SIZE];
static spi_hd_tx_desc_t s_tx_batch_desc[SPI_HD_TX_BATCH_BUFFERS];
static QueueHandle_t s_tx_free_slots, s_tx_pending_slots;
static TaskHandle_t s_tx_batch_task_handle;
#endif

#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
static bool spi_hd_is_batch_desc(const spi_hd_tx_desc_t *desc)
{
    for (unsigned int i = 0; i < SPI_HD_TX_BATCH_BUFFERS; i++) {
        if (desc == &s_tx_batch_desc[i])
            return true;
    }
    return false;
}
#endif

static interface_context_t context;
static interface_handle_t if_handle_g;
static QueueHandle_t spi_hd_rx_queue[MAX_PRIORITY_QUEUES];
static SemaphoreHandle_t spi_hd_rx_sem;
static SemaphoreHandle_t spi_hd_tx_sem;
static TaskHandle_t spi_hd_rx_task_handle;
static TaskHandle_t spi_hd_tx_done_task_handle;

static spi_hd_rx_slot_t s_rx_slots[SPI_HD_RX_QUEUE_SIZE];

/* DMA completions must refer only to descriptors handed to this driver.
 * Validate the pointer BEFORE reading any fields or freeing its buffers.
 */
static bool spi_hd_owned_rx_desc(const spi_slave_hd_data_t *trans)
{
    for (unsigned int i = 0; i < SPI_HD_RX_QUEUE_SIZE; i++) {
        if (trans == &s_rx_slots[i].trans)
            return true;
    }
    return false;
}

static uint8_t *s_frame_buf;
static size_t s_frame_len;
static size_t s_frame_expected;

static volatile bool s_running;
static bool s_driver_initialized;
#define SPI_HD_TX_INFLIGHT_MAX 1U
static spi_hd_tx_desc_t *s_tx_inflight[SPI_HD_TX_INFLIGHT_MAX];
static portMUX_TYPE s_tx_owner_lock = portMUX_INITIALIZER_UNLOCKED;

static bool spi_hd_add_tx_owner(spi_hd_tx_desc_t *desc)
{
    bool added = false;

    portENTER_CRITICAL(&s_tx_owner_lock);
    for (unsigned int i = 0; i < SPI_HD_TX_INFLIGHT_MAX; i++) {
        if (!s_tx_inflight[i]) {
            s_tx_inflight[i] = desc;
            added = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_tx_owner_lock);
    return added;
}

static bool spi_hd_release_tx_owner(spi_hd_tx_desc_t *desc)
{
    bool owned = false;

    portENTER_CRITICAL(&s_tx_owner_lock);
    for (unsigned int i = 0; i < SPI_HD_TX_INFLIGHT_MAX; i++) {
        if (s_tx_inflight[i] == desc) {
            s_tx_inflight[i] = NULL;
            owned = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_tx_owner_lock);
    return owned;
}
static uint32_t s_tx_ready_bytes;
static uint32_t s_rx_ready_segments;
/* Protect API admission and task-exit publication during teardown. */
static portMUX_TYPE s_lifecycle_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned int s_api_users;
#define SPI_HD_WAIT_TICKS pdMS_TO_TICKS(100)
#define SPI_HD_STOP_TICKS pdMS_TO_TICKS(3000)

static bool spi_hd_is_running(void)
{
    bool running;
    portENTER_CRITICAL(&s_lifecycle_lock);
    running = s_running;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return running;
}

static void spi_hd_set_running(bool running)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_running = running;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}

static bool spi_hd_api_enter(void)
{
    bool running;
    portENTER_CRITICAL(&s_lifecycle_lock);
    running = s_running;
    if (running)
        s_api_users++;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    return running;
}

static void spi_hd_api_leave(void)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    s_api_users--;
    portEXIT_CRITICAL(&s_lifecycle_lock);
}

static BaseType_t spi_hd_wait_sem(SemaphoreHandle_t sem)
{
    while (spi_hd_is_running()) {
        if (xSemaphoreTake(sem, SPI_HD_WAIT_TICKS) == pdTRUE) {
            if (spi_hd_is_running())
                return pdTRUE;
            xSemaphoreGive(sem);
            break;
        }
    }
    return pdFALSE;
}

static BaseType_t spi_hd_wait_queue_send(QueueHandle_t queue, const void *item)
{
    while (spi_hd_is_running()) {
        if (xQueueSend(queue, item, SPI_HD_WAIT_TICKS) == pdTRUE)
            return pdTRUE;
    }
    return pdFALSE;
}

static BaseType_t spi_hd_wait_queue_receive(QueueHandle_t queue, void *item)
{
    while (spi_hd_is_running()) {
        if (xQueueReceive(queue, item, SPI_HD_WAIT_TICKS) == pdTRUE)
            return pdTRUE;
    }
    return pdFALSE;
}

static void spi_hd_task_exit(TaskHandle_t *handle)
{
    portENTER_CRITICAL(&s_lifecycle_lock);
    *handle = NULL;
    portEXIT_CRITICAL(&s_lifecycle_lock);
    /* No transport resource may be accessed after publishing task exit. */
    vTaskDelete(NULL);
}

static interface_handle_t *esp_spi_hd_init(void);
static int32_t esp_spi_hd_write(interface_handle_t *handle,
                                interface_buffer_handle_t *buf_handle);
static int esp_spi_hd_read(interface_handle_t *handle,
                           interface_buffer_handle_t *buf_handle);
static esp_err_t esp_spi_hd_reset(interface_handle_t *handle);
static void esp_spi_hd_deinit(interface_handle_t *handle);

static if_ops_t if_ops = {
    .init = esp_spi_hd_init,
    .write = esp_spi_hd_write,
    .read = esp_spi_hd_read,
    .reset = esp_spi_hd_reset,
    .deinit = esp_spi_hd_deinit,
};

static void spi_hd_set_data_ready(bool active)
{
    gpio_set_level(GPIO_DATA_READY, active ? 1 : 0);
}

static void spi_hd_write_reg32(uint32_t addr, uint32_t value)
{
    spi_slave_hd_write_buffer(SPI_HD_HOST, addr,
                              (uint8_t *)&value, sizeof(value));
}

/* Called only from firmware tasks, never DMA/ISR callbacks. No protocol
 * command can recover an unknown byte boundary or an orphaned descriptor:
 * invalidate the advertised incarnation and restart. The host will
 * reconstruct only after receiving a new boot event with reset counters.
 */
static void spi_hd_transport_fatal(const char *reason, esp_err_t err)
{
    ESP_LOGE(TAG, "SPI-HD transport fatal: %s (%s); restarting firmware",
             reason, esp_err_to_name(err));
    spi_hd_set_running(false);
    spi_hd_set_data_ready(false);
    if (s_driver_initialized)
        spi_hd_write_reg32(SPIHD_REG_SLAVE_READY, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_restart();
}

static bool spi_hd_cb_tx_ready(void *arg, spi_slave_hd_event_t *event,
                               int *awoken)
{
    uint32_t int_mask;

    (void)arg;
    (void)awoken;

    if (!event || !event->trans)
        return false;

    int_mask = s_tx_ready_bytes & ~SPIHD_TX_BUF_LEN_MASK;
#ifdef CONFIG_ESP_SPI_HD_TX_LEN_MIRROR
    int_mask = (int_mask & 0x03000000U) |
               ((int_mask + 0x04000000U) & 0xFC000000U);
#endif
    s_tx_ready_bytes =
        ((s_tx_ready_bytes & SPIHD_TX_BUF_LEN_MASK) + event->trans->len) &
        SPIHD_TX_BUF_LEN_MASK;
    s_tx_ready_bytes |= int_mask;
#ifdef CONFIG_ESP_SPI_HD_TX_LEN_MIRROR
    spi_hd_write_reg32(SPIHD_REG_TX_BUF_LEN, s_tx_ready_bytes & 0x03FFFFFFU);
    /* The spare six bits tag each publication without another RAM counter.
     * Read low then high; publish high then low using aligned word stores.
     */
    spi_hd_write_reg32(SPIHD_REG_TX_LEN_MIRROR + 4, s_tx_ready_bytes);
    spi_hd_write_reg32(SPIHD_REG_TX_LEN_MIRROR, s_tx_ready_bytes);
#else
    spi_hd_write_reg32(SPIHD_REG_TX_BUF_LEN, s_tx_ready_bytes);
#endif
    spi_hd_set_data_ready(true);
    return true;
}

static bool spi_hd_cb_rx_ready(void *arg, spi_slave_hd_event_t *event,
                               int *awoken)
{
    (void)arg;
    (void)awoken;
    if (!event || !event->trans)
        return false;

    s_rx_ready_segments++;
    spi_hd_write_reg32(SPIHD_REG_RX_BUF_LEN, s_rx_ready_segments);
    return true;
}

#ifndef CONFIG_ESP_SPI_HD_DMA_APPEND
static bool spi_hd_cb_cmd9(void *arg, spi_slave_hd_event_t *event,
                           int *awoken)
{
    (void)arg;
    (void)event;
    (void)awoken;

    spi_hd_set_data_ready(false);
    return true;
}

#endif

#ifdef CONFIG_ESP_SPI_HD_DMA_APPEND
static bool spi_hd_cb_append_tx_done(void *arg, spi_slave_hd_event_t *event,
                                     int *awoken)
{
    (void)arg;
    (void)event;
    (void)awoken;
    /* Append mode has GDMA completion callbacks, not CMD9 SPI callbacks. */
    spi_hd_set_data_ready(false);
    return true;
}
#endif

static esp_err_t spi_hd_queue_dma(spi_slave_chan_t chan,
                                  spi_slave_hd_data_t *trans,
                                  TickType_t timeout)
{
#ifdef CONFIG_ESP_SPI_HD_DMA_APPEND
    esp_err_t ret = spi_slave_hd_append_trans(SPI_HD_HOST, chan, trans, timeout);
    if (ret == ESP_OK) {
        spi_slave_hd_event_t event = { .trans = trans };
        int awoken = 0;
        /* Append mode does not invoke *_dma_ready. Publish only after the
         * descriptor has been linked; RX credits now cover the full chain.
         */
        if (chan == SPI_SLAVE_CHAN_RX)
            spi_hd_cb_rx_ready(NULL, &event, &awoken);
        else
            spi_hd_cb_tx_ready(NULL, &event, &awoken);
    }
    return ret;
#else
    return spi_slave_hd_queue_trans(SPI_HD_HOST, chan, trans, timeout);
#endif
}

static esp_err_t spi_hd_get_dma_result(spi_slave_chan_t chan,
                                       spi_slave_hd_data_t **trans,
                                       TickType_t timeout)
{
#ifdef CONFIG_ESP_SPI_HD_DMA_APPEND
    return spi_slave_hd_get_append_trans_res(SPI_HD_HOST, chan, trans, timeout);
#else
    return spi_slave_hd_get_trans_res(SPI_HD_HOST, chan, trans, timeout);
#endif
}

static void spi_hd_reset_assembly(void)
{
    if (s_frame_buf)
        heap_caps_free(s_frame_buf);
    s_frame_buf = NULL;
    s_frame_len = 0;
    s_frame_expected = 0;
}

static bool spi_hd_validate_complete_frame(uint8_t *frame, size_t frame_len,
                                           interface_buffer_handle_t *out)
{
    struct esp_payload_header *header;
    uint16_t len;
    uint16_t offset;
#if CONFIG_ESP_SPI_HD_CHECKSUM
    uint16_t wire_checksum;
    uint16_t checksum;
#endif

    if (!frame || frame_len < sizeof(struct esp_payload_header) || !out)
        return false;

    header = (struct esp_payload_header *)frame;
    if (header->if_type >= ESP_MAX_IF)
        return false;

    len = le16toh(header->len);
    offset = le16toh(header->offset);
    if (!len || !ESP_OFFSET_VALID(offset) || offset > frame_len ||
        len > frame_len - offset || (size_t)(offset + len) != frame_len)
        return false;

#if CONFIG_ESP_SPI_HD_CHECKSUM
    wire_checksum = le16toh(header->checksum);
    header->checksum = 0;
    checksum = compute_checksum(frame, frame_len);
    header->checksum = htole16(wire_checksum);
    if (checksum != wire_checksum) {
        ESP_LOGW(TAG, "drop frame: checksum mismatch wire=0x%04x calc=0x%04x",
                 wire_checksum, checksum);
        return false;
    }
#endif

    memset(out, 0, sizeof(*out));
    out->if_type = header->if_type;
    out->if_num = header->if_num;
    out->payload = frame;
    out->payload_len = frame_len;
    out->priv_buffer_handle = frame;
    out->free_buf_handle = heap_caps_free;
    return true;
}

static bool spi_hd_update_expected(void)
{
    struct esp_payload_header *header;
    uint16_t len;
    uint16_t offset;

    if (s_frame_expected)
        return true;
    if (s_frame_len < sizeof(struct esp_payload_header))
        return true;

    header = (struct esp_payload_header *)s_frame_buf;
    len = le16toh(header->len);
    offset = le16toh(header->offset);

    if (!len || !ESP_OFFSET_VALID(offset)) {
        ESP_LOGW(TAG, "drop segmented frame: bad header len=%u offset=%u",
                 len, offset);
        return false;
    }

    s_frame_expected = (size_t)offset + len;
    if (s_frame_expected > SPI_HD_FRAME_BUF_SIZE ||
        s_frame_expected < sizeof(struct esp_payload_header)) {
        ESP_LOGW(TAG, "drop segmented frame: expected=%u",
                 (unsigned int)s_frame_expected);
        return false;
    }
    return true;
}

static int spi_hd_queue_rx_frame(interface_buffer_handle_t *frame)
{
    QueueHandle_t queue;
    BaseType_t ret;

    if (frame->if_type == ESP_INTERNAL_IF)
        queue = spi_hd_rx_queue[PRIO_Q_HIGH];
    else if (frame->if_type == ESP_HCI_IF || frame->if_type == ESP_RCP_IF)
        queue = spi_hd_rx_queue[PRIO_Q_MID];
    else
        queue = spi_hd_rx_queue[PRIO_Q_LOW];

    ret = spi_hd_wait_queue_send(queue, frame);
    if (ret != pdTRUE)
        return ESP_FAIL;

    xSemaphoreGive(spi_hd_rx_sem);
    return ESP_OK;
}

static void spi_hd_consume_segment(const uint8_t *data, size_t len)
{
    if (len & (SPIHD_FRAME_ALIGNMENT - 1)) {
        spi_hd_transport_fatal("unaligned RX DMA segment", ESP_ERR_INVALID_SIZE);
        return;
    }
    while (data && len) {
        interface_buffer_handle_t frame = {0};
        size_t target, count, padding;

        if (!s_frame_buf) {
            s_frame_buf = heap_caps_malloc(SPI_HD_FRAME_BUF_SIZE, MALLOC_CAP_8BIT);
            if (!s_frame_buf) {
                spi_hd_transport_fatal("frame assembly allocation", ESP_ERR_NO_MEM);
                return;
            }
        }
        target = s_frame_expected ? s_frame_expected : sizeof(struct esp_payload_header);
        count = target - s_frame_len;
        if (count > len)
            count = len;
        memcpy(s_frame_buf + s_frame_len, data, count);
        s_frame_len += count;
        data += count;
        len -= count;

        if (!spi_hd_update_expected()) {
            spi_hd_reset_assembly();
            spi_hd_transport_fatal("invalid segmented frame header", ESP_ERR_INVALID_SIZE);
            return;
        }
        if (!s_frame_expected || s_frame_len < s_frame_expected)
            continue;
        if (!spi_hd_validate_complete_frame(s_frame_buf, s_frame_len, &frame)) {
            spi_hd_reset_assembly();
            spi_hd_transport_fatal("frame validation or checksum failure", ESP_ERR_INVALID_CRC);
            return;
        }
        padding = SPIHD_WIRE_SPAN(s_frame_len) - s_frame_len;
        if (padding > len) {
            spi_hd_reset_assembly();
            spi_hd_transport_fatal("missing frame padding", ESP_ERR_INVALID_SIZE);
            return;
        }
        for (size_t i = 0; i < padding; i++) {
            if (data[i]) {
                spi_hd_reset_assembly();
                spi_hd_transport_fatal("nonzero frame padding", ESP_ERR_INVALID_SIZE);
                return;
            }
        }
        data += padding;
        len -= padding;
        s_frame_buf = NULL;
        s_frame_len = 0;
        s_frame_expected = 0;
        if (spi_hd_queue_rx_frame(&frame) != ESP_OK) {
            if (frame.free_buf_handle && frame.priv_buffer_handle)
                frame.free_buf_handle(frame.priv_buffer_handle);
            if (spi_hd_is_running())
                spi_hd_transport_fatal("hosted RX queue ownership lost", ESP_FAIL);
            return;
        }
    }
}

static void spi_hd_rx_task(void *arg)
{
    spi_slave_hd_data_t *ret_trans = NULL;
    uint32_t ctrl = 0;
    esp_err_t ret;
    int i;
    TickType_t last_idle_window = xTaskGetTickCount();

    (void)arg;

    /*
     * SLAVE_READY covers register access only. Do not publish RX DMA credits
     * or open the common datapath until the host explicitly writes
     * SLAVE_CTRL.DATAPATH_ON, matching the current esp-hosted-mcu SPI-HD
     * handshake.
     */
    while (spi_hd_is_running()) {
        spi_slave_hd_read_buffer(SPI_HD_HOST, SPIHD_REG_SLAVE_CTRL,
                                 (uint8_t *)&ctrl, sizeof(ctrl));
        if (ctrl & SPIHD_CTRL_DATAPATH_ON)
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!spi_hd_is_running())
        goto out;

    spi_slave_hd_read_buffer(SPI_HD_HOST, SPIHD_REG_HOST_RX_MAX,
                             (uint8_t *)&s_host_rx_max, sizeof(s_host_rx_max));
    if (s_host_rx_max < SPI_HD_FRAME_BUF_SIZE ||
        s_host_rx_max > SPI_HD_FRAME_BUF_SIZE * 2U) {
        spi_hd_transport_fatal("invalid host receive capacity", ESP_ERR_INVALID_SIZE);
        goto out;
    }

    for (i = 0; i < SPI_HD_RX_QUEUE_SIZE; i++) {
        ret = spi_hd_queue_dma(SPI_SLAVE_CHAN_RX,
                               &s_rx_slots[i].trans, SPI_HD_WAIT_TICKS);
        if (ret != ESP_OK) {
            if (spi_hd_is_running())
                spi_hd_transport_fatal("initial RX DMA descriptor enqueue", ret);
            goto out;
        }
    }

    /*
     * init() publishes SLAVE_READY just before returning. Give app_main a
     * scheduling window to store if_handle before the event callback touches
     * it; this mirrors the reference SPI-HD transport's post-open settle.
     */
#ifdef CONFIG_ESP_SPI_HD_DMA_APPEND
    ESP_LOGI(TAG, "SPI-HD RX append preload complete: credits=%" PRIu32,
             s_rx_ready_segments);
#endif
    vTaskDelay(pdMS_TO_TICKS(100));
    if (spi_hd_is_running() && context.event_handler)
        context.event_handler(ESP_OPEN_DATA_PATH);

    while (spi_hd_is_running()) {
        ret_trans = NULL;
        ret = spi_hd_get_dma_result(SPI_SLAVE_CHAN_RX,
                                    &ret_trans, pdMS_TO_TICKS(100));
        if (ret == ESP_ERR_TIMEOUT)
            continue;
        if (ret != ESP_OK || !ret_trans) {
            if (!spi_hd_is_running())
                break;
            /* Descriptor ownership is unknown on a non-timeout result.
             * Do not continue advertising RX credits with a dead queue.
             */
            spi_hd_transport_fatal("RX DMA completion", ret != ESP_OK ? ret : ESP_FAIL);
            break;
        }
        if (!spi_hd_owned_rx_desc(ret_trans)) {
            spi_hd_transport_fatal("RX DMA returned an unknown descriptor", ESP_FAIL);
            break;
        }
        /* Descriptor identity alone is insufficient if the DMA provider
         * overwrote its data/length fields. Validate ownership and bounds
         * before accessing the buffer.
         */
        if (ret_trans->data != ((spi_hd_rx_slot_t *)ret_trans)->buf ||
            ret_trans->len != SPI_HD_RX_SEGMENT_SIZE) {
            spi_hd_transport_fatal("RX DMA buffer ownership/size mismatch", ESP_FAIL);
            break;
        }
        /* DMA completion metadata must never index beyond its buffer.
         * Reject impossible lengths before assembly.
         */
        if (ret_trans->trans_len > ret_trans->len) {
            ESP_LOGE(TAG, "SPI-HD RX DMA length overflow: actual=%u allocated=%u",
                     (unsigned int)ret_trans->trans_len,
                     (unsigned int)ret_trans->len);
            spi_hd_reset_assembly();
            spi_hd_transport_fatal("RX DMA descriptor exceeded its allocation",
                                   ESP_ERR_INVALID_SIZE);
            break;
        }

        if (!ret_trans->trans_len) {
            spi_hd_transport_fatal("empty RX DMA completion", ESP_ERR_INVALID_SIZE);
            break;
        }

        spi_hd_consume_segment(ret_trans->data, ret_trans->trans_len);
        if (!spi_hd_is_running())
            break;

        /* Reuse the descriptor only after its payload has been consumed. */
        ret_trans->trans_len = 0;
        ret = spi_hd_queue_dma(SPI_SLAVE_CHAN_RX,
                               ret_trans, SPI_HD_WAIT_TICKS);
        if (ret != ESP_OK) {
            if (spi_hd_is_running())
                spi_hd_transport_fatal("RX DMA descriptor requeue", ret);
            break;
        }
        /* Continuous RX must leave an idle-task window for the watchdog.
         * Yield after rearming DMA, at most once a second. taskYIELD alone
         * cannot run a lower-priority idle task while RX remains ready.
         */
        if ((TickType_t)(xTaskGetTickCount() - last_idle_window) >=
            pdMS_TO_TICKS(1000)) {
            vTaskDelay(1);
            last_idle_window = xTaskGetTickCount();
        }
    }

out:
    spi_hd_task_exit(&spi_hd_rx_task_handle);
}

static void spi_hd_tx_done_task(void *arg)
{
    spi_slave_hd_data_t *ret_trans = NULL;
    spi_hd_tx_desc_t *desc;
    esp_err_t ret;

    (void)arg;

    while (spi_hd_is_running()) {
        ret_trans = NULL;
        ret = spi_hd_get_dma_result(SPI_SLAVE_CHAN_TX,
                                    &ret_trans, pdMS_TO_TICKS(100));
        if (ret == ESP_ERR_TIMEOUT)
            continue;
        if (ret != ESP_OK || !ret_trans) {
            if (!spi_hd_is_running())
                break;
            /* This queue has only one DMA owner. Ignoring a completion
             * failure can strand the semaphore and all future E2H TX.
             */
            spi_hd_transport_fatal("TX DMA completion", ret != ESP_OK ? ret : ESP_FAIL);
            break;
        }

        desc = (spi_hd_tx_desc_t *)ret_trans;
        if (!spi_hd_release_tx_owner(desc)) {
            spi_hd_transport_fatal("TX DMA returned an unowned descriptor", ESP_FAIL);
            break;
        }
        if (desc->raw_tp)
            debug_raw_tp_tx_complete(desc->raw_tp_frames);
#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
        if (spi_hd_is_batch_desc(desc)) {
        } else
#endif
        {
            heap_caps_free(desc->buf);
            free(desc);
        }
        xSemaphoreGive(spi_hd_tx_sem);
    }

    spi_hd_task_exit(&spi_hd_tx_done_task_handle);
}

/* Takes ownership of the DMA-capable frame on every return path. */
static int spi_hd_queue_frame(uint8_t *frame, size_t len,
                              bool raw_tp, uint32_t raw_tp_run_id)
{
    spi_hd_tx_desc_t *desc;
    esp_err_t ret;

    if (!spi_hd_is_running() || !frame || !len || len > SPI_HD_FRAME_BUF_SIZE) {
        heap_caps_free(frame);
        return ESP_FAIL;
    }

    if (spi_hd_wait_sem(spi_hd_tx_sem) != pdTRUE) {
        heap_caps_free(frame);
        return ESP_FAIL;
    }

    desc = calloc(1, sizeof(*desc));
    if (!desc) {
        heap_caps_free(frame);
        xSemaphoreGive(spi_hd_tx_sem);
        return ESP_ERR_NO_MEM;
    }

    desc->buf = frame;
    desc->trans.data = desc->buf;
    desc->trans.len = len;
    desc->raw_tp = raw_tp;
    desc->raw_tp_run_id = raw_tp_run_id;
    desc->raw_tp_frames = raw_tp ? 1 : 0;

    /* Track ownership before exposing the descriptor to DMA. */
    if (!spi_hd_add_tx_owner(desc)) {
        heap_caps_free(desc->buf);
        free(desc);
        xSemaphoreGive(spi_hd_tx_sem);
        return ESP_FAIL;
    }
    ret = spi_hd_queue_dma(SPI_SLAVE_CHAN_TX,
                           &desc->trans, SPI_HD_WAIT_TICKS);
    if (ret != ESP_OK) {
        spi_hd_release_tx_owner(desc);
        heap_caps_free(desc->buf);
        free(desc);
        xSemaphoreGive(spi_hd_tx_sem);
        return ret;
    }

    return ESP_OK;
}

#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
static void spi_hd_tx_batch_task(void *arg)
{
    spi_hd_tx_slot_t *slot, *next;
    unsigned int frames, logs = 0;
    spi_hd_tx_desc_t *desc;
    esp_err_t ret;
    (void)arg;

    while (spi_hd_is_running()) {
        /* Wait for input before taking the TX semaphore: boot events must
         * still be able to transmit while this queue is empty.
         */
        if (xQueueReceive(s_tx_pending_slots, &slot, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        desc = &s_tx_batch_desc[0];
        if (spi_hd_wait_sem(spi_hd_tx_sem) != pdTRUE)
            break;
        desc->trans.len = 0;
        desc->raw_tp = slot->raw_tp;
        desc->raw_tp_run_id = slot->run_id;
        desc->raw_tp_frames = 0;
        frames = 0;
        for (;;) {
            memcpy(desc->buf + desc->trans.len,
                   slot->buf, slot->len);
            desc->trans.len += slot->len;
            if (slot->raw_tp)
                desc->raw_tp_frames++;
            frames++;
            /* The pool buffers stay allocated until driver quiescence;
             * returning a slot does not alter the DMA aggregate.
             */
            xQueueSend(s_tx_free_slots, &slot, 0);
            if (frames >= SPI_HD_TX_BATCH_FRAMES ||
                xQueuePeek(s_tx_pending_slots, &next, 0) != pdTRUE ||
                next->len > s_host_rx_max - desc->trans.len ||
                next->raw_tp != desc->raw_tp ||
                next->run_id != desc->raw_tp_run_id)
                break;
            if (xQueueReceive(s_tx_pending_slots, &slot, 0) != pdTRUE)
                break;
        }
        desc->trans.data = desc->buf;
        if (!spi_hd_add_tx_owner(desc))
            ret = ESP_FAIL;
        else
            ret = spi_hd_queue_dma(SPI_SLAVE_CHAN_TX,
                                  &desc->trans, SPI_HD_WAIT_TICKS);
        if (ret != ESP_OK) {
            spi_hd_release_tx_owner(desc);
            if (desc->raw_tp)
                debug_raw_tp_tx_failed(desc->raw_tp_frames);
            ESP_LOGE(TAG, "TX batch queue failed: %s", esp_err_to_name(ret));
            xSemaphoreGive(spi_hd_tx_sem);
            if (spi_hd_is_running())
                spi_hd_transport_fatal("TX batch DMA queue", ret);
            break;
        }
        if (frames > 1 && logs++ < 3)
            ESP_LOGI(TAG, "SPI-HD TX batch: frames=%u bytes=%u",
                     frames, (unsigned int)desc->trans.len);
    }
    spi_hd_task_exit(&s_tx_batch_task_handle);
}
#endif

static int32_t spi_hd_write_active(interface_handle_t *handle,
                                  interface_buffer_handle_t *buf_handle)
{
    struct esp_payload_header *header;
    uint8_t *frame;
    uint16_t offset = sizeof(struct esp_payload_header);
    size_t total_len, wire_len;
    bool raw_tp = false;
    esp_err_t ret;
#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
    spi_hd_tx_slot_t *slot;
#endif

    if (!spi_hd_is_running() || !handle || handle->state != ACTIVE || !buf_handle ||
        !buf_handle->payload || !buf_handle->payload_len)
        return ESP_FAIL;

    total_len = offset + buf_handle->payload_len;
    if (total_len > SPI_HD_FRAME_BUF_SIZE)
        return ESP_FAIL;
    wire_len = SPIHD_WIRE_SPAN(total_len);

#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
    if (spi_hd_wait_queue_receive(s_tx_free_slots, &slot) != pdTRUE)
        return ESP_FAIL;
    frame = slot->buf;
    memset(frame, 0, wire_len);
#else
    frame = heap_caps_calloc(1, wire_len, MALLOC_CAP_DMA);
    if (!frame)
        return ESP_ERR_NO_MEM;
#endif

    header = (struct esp_payload_header *)frame;
    header->if_type = buf_handle->if_type;
    header->if_num = buf_handle->if_num;
    header->len = htole16(buf_handle->payload_len);
    header->offset = htole16(offset);
    header->flags = buf_handle->flag;
    header->packet_type = buf_handle->pkt_type;
    if (header->if_type == ESP_TEST_IF) {
        debug_raw_tp_set_seq(header, buf_handle->raw_tp_seq);
        raw_tp = true;
    }

    memcpy(frame + offset, buf_handle->payload, buf_handle->payload_len);

#if CONFIG_ESP_SPI_HD_CHECKSUM
    header->checksum = htole16(compute_checksum(frame, total_len));
#endif

#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
    slot->len = wire_len;
    slot->raw_tp = raw_tp;
    slot->run_id = raw_tp ? buf_handle->raw_tp_run_id : 0;
    ret = spi_hd_wait_queue_send(s_tx_pending_slots, &slot) == pdTRUE ?
          ESP_OK : ESP_FAIL;
    if (ret != ESP_OK)
        xQueueSend(s_tx_free_slots, &slot, 0);
#else
    ret = spi_hd_queue_frame(frame, wire_len, raw_tp,
                             buf_handle->raw_tp_run_id);
#endif

    if (ret != ESP_OK && raw_tp)
        debug_raw_tp_tx_failed(1);
    return ret == ESP_OK ? buf_handle->payload_len : ESP_FAIL;
}

static int spi_hd_read_active(interface_handle_t *handle,
                              interface_buffer_handle_t *buf_handle)
{
    BaseType_t ret = pdFALSE;

    if (!spi_hd_is_running() || !handle || handle->state != ACTIVE || !buf_handle)
        return ESP_FAIL;

    if (spi_hd_wait_sem(spi_hd_rx_sem) != pdTRUE)
        return ESP_FAIL;

    if (uxQueueMessagesWaiting(spi_hd_rx_queue[PRIO_Q_HIGH]))
        ret = xQueueReceive(spi_hd_rx_queue[PRIO_Q_HIGH], buf_handle, 0);
    if (ret != pdTRUE && uxQueueMessagesWaiting(spi_hd_rx_queue[PRIO_Q_MID]))
        ret = xQueueReceive(spi_hd_rx_queue[PRIO_Q_MID], buf_handle, 0);
    if (ret != pdTRUE && uxQueueMessagesWaiting(spi_hd_rx_queue[PRIO_Q_LOW]))
        ret = xQueueReceive(spi_hd_rx_queue[PRIO_Q_LOW], buf_handle, 0);

    return ret == pdTRUE ? buf_handle->payload_len : ESP_FAIL;
}

static int32_t esp_spi_hd_write(interface_handle_t *handle,
                                interface_buffer_handle_t *buf_handle)
{
    int32_t ret;
    if (!spi_hd_api_enter())
        return ESP_FAIL;
    ret = spi_hd_write_active(handle, buf_handle);
    spi_hd_api_leave();
    return ret;
}

static int esp_spi_hd_read(interface_handle_t *handle,
                           interface_buffer_handle_t *buf_handle)
{
    int ret;
    if (!spi_hd_api_enter())
        return ESP_FAIL;
    ret = spi_hd_read_active(handle, buf_handle);
    spi_hd_api_leave();
    return ret;
}

static void spi_hd_bus_cfg(spi_bus_config_t *bus_cfg)
{
    memset(bus_cfg, 0, sizeof(*bus_cfg));
    bus_cfg->data0_io_num = GPIO_D0;
    bus_cfg->data1_io_num = GPIO_D1;
    bus_cfg->data2_io_num = GPIO_D2;
    bus_cfg->data3_io_num = GPIO_D3;
    bus_cfg->data4_io_num = -1;
    bus_cfg->data5_io_num = -1;
    bus_cfg->data6_io_num = -1;
    bus_cfg->data7_io_num = -1;
    bus_cfg->sclk_io_num = GPIO_SCLK;
    bus_cfg->max_transfer_sz = SPI_HD_RX_SEGMENT_SIZE > SPI_HD_FRAME_BUF_SIZE ?
                               SPI_HD_RX_SEGMENT_SIZE : SPI_HD_FRAME_BUF_SIZE;
#ifdef CONFIG_ESP_SPI_HD_DMA_APPEND
    /* IDF sizes the DMA descriptor pool as ceil(max_transfer_sz/4092),
     * independently of queue_size. Each queued RX segment needs one
     * descriptor, including short WRDMA transactions terminated by WR_END.
     * This capacity does not change our advertised/frame byte limit.
     */
    bus_cfg->max_transfer_sz = SPI_MAX_DMA_LEN * SPI_HD_RX_QUEUE_SIZE;
#endif
    bus_cfg->flags = SPICOMMON_BUSFLAG_QUAD;
    bus_cfg->intr_flags = 0;
}

static void spi_hd_slot_cfg(spi_slave_hd_slot_config_t *slot_cfg)
{
    memset(slot_cfg, 0, sizeof(*slot_cfg));
    slot_cfg->spics_io_num = GPIO_CS;
    slot_cfg->mode = CONFIG_ESP_SPI_HD_MODE;
    slot_cfg->command_bits = 8;
    slot_cfg->address_bits = 8;
    slot_cfg->dummy_bits = 8;
    slot_cfg->queue_size = SPI_HD_RX_QUEUE_SIZE;
    slot_cfg->dma_chan = SPI_HD_DMA_CHAN;
    slot_cfg->cb_config = (spi_slave_hd_callback_config_t) {
#ifndef CONFIG_ESP_SPI_HD_DMA_APPEND
        .cb_send_dma_ready = spi_hd_cb_tx_ready,
        .cb_recv_dma_ready = spi_hd_cb_rx_ready,
        .cb_cmd9 = spi_hd_cb_cmd9,
#else
        .cb_sent = spi_hd_cb_append_tx_done,
#endif
    };
#ifdef CONFIG_ESP_SPI_HD_DMA_APPEND
    slot_cfg->flags |= SPI_SLAVE_HD_APPEND_MODE;
    ESP_LOGI(TAG, "SPI-HD append DMA enabled: preloaded RX descriptors=%u",
             (unsigned int)SPI_HD_RX_QUEUE_SIZE);
#endif
}

static void spi_hd_cleanup_resources(void)
{
    interface_buffer_handle_t frame;
    uint8_t q;
    int i;

    TickType_t started = xTaskGetTickCount();
    bool busy;

    spi_hd_set_running(false);
    /* Stop admission, then allow bounded waits to finish naturally. Never
     * delete a task inside a DMA API or free resources beneath a caller. */
    do {
        portENTER_CRITICAL(&s_lifecycle_lock);
        busy = s_api_users || spi_hd_rx_task_handle || spi_hd_tx_done_task_handle;
#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
        busy = busy || s_tx_batch_task_handle;
#endif
        portEXIT_CRITICAL(&s_lifecycle_lock);
        if (!busy)
            break;
        if ((TickType_t)(xTaskGetTickCount() - started) >= SPI_HD_STOP_TICKS) {
            ESP_LOGE(TAG, "SPI-HD shutdown did not quiesce; restarting");
            esp_restart();
            return;
        }
        vTaskDelay(1);
    } while (true);

    spi_hd_set_data_ready(false);
    if (s_driver_initialized) {
        spi_hd_write_reg32(SPIHD_REG_SLAVE_READY, 0);
        if (spi_slave_hd_deinit(SPI_HD_HOST) != ESP_OK) {
            ESP_LOGE(TAG, "SPI-HD DMA deinit failed; restarting");
            esp_restart();
            return;
        }
        s_driver_initialized = false;
    }

    /*
     * A queued TX descriptor is owned by this transport, not by the IDF
     * driver. Once spi_slave_hd_deinit() has returned it is safe to reclaim
     * an unfinished descriptor that never reached the completion task.
     */
    for (unsigned int i = 0; i < SPI_HD_TX_INFLIGHT_MAX; i++) {
        spi_hd_tx_desc_t *desc = s_tx_inflight[i];
        if (!desc)
            continue;
        if (desc->raw_tp)
            debug_raw_tp_tx_failed(desc->raw_tp_frames);
#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
        if (!spi_hd_is_batch_desc(desc)) {
#endif
            heap_caps_free(desc->buf);
            free(desc);
#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
        }
#endif
        s_tx_inflight[i] = NULL;
    }

    for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
        if (!spi_hd_rx_queue[q])
            continue;
        while (xQueueReceive(spi_hd_rx_queue[q], &frame, 0) == pdTRUE) {
            if (frame.free_buf_handle && frame.priv_buffer_handle)
                frame.free_buf_handle(frame.priv_buffer_handle);
        }
        vQueueDelete(spi_hd_rx_queue[q]);
        spi_hd_rx_queue[q] = NULL;
    }

    if (spi_hd_rx_sem) {
        vSemaphoreDelete(spi_hd_rx_sem);
        spi_hd_rx_sem = NULL;
    }
    if (spi_hd_tx_sem) {
        vSemaphoreDelete(spi_hd_tx_sem);
        spi_hd_tx_sem = NULL;
    }

    for (i = 0; i < SPI_HD_RX_QUEUE_SIZE; i++) {
        heap_caps_free(s_rx_slots[i].buf);
        s_rx_slots[i].buf = NULL;
        memset(&s_rx_slots[i].trans, 0, sizeof(s_rx_slots[i].trans));
    }

#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
    if (s_tx_free_slots) {
        vQueueDelete(s_tx_free_slots);
        s_tx_free_slots = NULL;
    }
    if (s_tx_pending_slots) {
        vQueueDelete(s_tx_pending_slots);
        s_tx_pending_slots = NULL;
    }
    for (i = 0; i < SPI_HD_TX_POOL_SIZE; i++) {
        heap_caps_free(s_tx_pool[i].buf);
        memset(&s_tx_pool[i], 0, sizeof(s_tx_pool[i]));
    }
    for (i = 0; i < SPI_HD_TX_BATCH_BUFFERS; i++) {
        heap_caps_free(s_tx_batch_desc[i].buf);
        memset(&s_tx_batch_desc[i], 0, sizeof(s_tx_batch_desc[i]));
    }
#endif
    spi_hd_reset_assembly();
    s_tx_ready_bytes = 0;
    s_rx_ready_segments = 0;
    s_host_rx_max = 0;
    memset(&if_handle_g, 0, sizeof(if_handle_g));
}

static interface_handle_t *esp_spi_hd_init(void)
{
    spi_bus_config_t bus_cfg;
    spi_slave_hd_slot_config_t slot_cfg;
    gpio_config_t dr_cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << GPIO_DATA_READY,
    };
    uint8_t zero_regs[SPIHD_REGISTER_BYTES] = {0};
    uint32_t value, session_lo, session_hi;
    uint8_t q;
    int i;
    esp_err_t ret;

    spi_hd_set_running(false);
    s_driver_initialized = false;
    memset(s_tx_inflight, 0, sizeof(s_tx_inflight));
    spi_hd_reset_assembly();

    ESP_LOGI(TAG,
             "SPI-HD/QIO CLK=%d CS=%d D0=%d D1=%d D2=%d D3=%d DR=%d mode=%d",
             GPIO_SCLK, GPIO_CS, GPIO_D0, GPIO_D1, GPIO_D2, GPIO_D3,
             GPIO_DATA_READY, CONFIG_ESP_SPI_HD_MODE);

    ret = gpio_config(&dr_cfg);
    if (ret != ESP_OK)
        return NULL;
    spi_hd_set_data_ready(false);
    ret = gpio_set_pull_mode(GPIO_DATA_READY, GPIO_PULLDOWN_ONLY);
    if (ret != ESP_OK)
        return NULL;
    ret = gpio_set_pull_mode(GPIO_SCLK, GPIO_PULLUP_ONLY);
    if (ret != ESP_OK)
        return NULL;
    ret = gpio_set_pull_mode(GPIO_CS, GPIO_PULLUP_ONLY);
    if (ret != ESP_OK)
        return NULL;

    spi_hd_bus_cfg(&bus_cfg);
    spi_hd_slot_cfg(&slot_cfg);

    ret = spi_slave_hd_init(SPI_HD_HOST, &bus_cfg, &slot_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_hd_init failed: %s", esp_err_to_name(ret));
        return NULL;
    }
    s_driver_initialized = true;

    spi_hd_rx_sem = xSemaphoreCreateCounting(
        SPI_HD_RX_QUEUE_SIZE * MAX_PRIORITY_QUEUES, 0);
    spi_hd_tx_sem = xSemaphoreCreateCounting(SPI_HD_TX_INFLIGHT_MAX, SPI_HD_TX_INFLIGHT_MAX);
    if (!spi_hd_rx_sem || !spi_hd_tx_sem)
        goto fail;

    for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
        spi_hd_rx_queue[q] =
            xQueueCreate(SPI_HD_RX_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
        if (!spi_hd_rx_queue[q])
            goto fail;
    }

    s_tx_ready_bytes = 0;
    s_rx_ready_segments = 0;
    spi_slave_hd_write_buffer(SPI_HD_HOST, 0, zero_regs, sizeof(zero_regs));

    memset(&if_handle_g, 0, sizeof(if_handle_g));
    if_handle_g.state = INIT;

    for (i = 0; i < SPI_HD_RX_QUEUE_SIZE; i++) {
        s_rx_slots[i].buf =
            heap_caps_malloc(SPI_HD_RX_SEGMENT_SIZE, MALLOC_CAP_DMA);
        if (!s_rx_slots[i].buf)
            goto fail;
        memset(&s_rx_slots[i].trans, 0, sizeof(s_rx_slots[i].trans));
        s_rx_slots[i].trans.data = s_rx_slots[i].buf;
        s_rx_slots[i].trans.len = SPI_HD_RX_SEGMENT_SIZE;
    }

#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
    s_tx_free_slots = xQueueCreate(SPI_HD_TX_POOL_SIZE, sizeof(spi_hd_tx_slot_t *));
    s_tx_pending_slots = xQueueCreate(SPI_HD_TX_POOL_SIZE, sizeof(spi_hd_tx_slot_t *));
    if (!s_tx_free_slots || !s_tx_pending_slots)
        goto fail;
    for (i = 0; i < SPI_HD_TX_BATCH_BUFFERS; i++) {
        s_tx_batch_desc[i].buf = heap_caps_malloc(SPI_HD_TX_BATCH_SIZE, MALLOC_CAP_DMA);
        if (!s_tx_batch_desc[i].buf)
            goto fail;
    }
    ESP_LOGI(TAG, "SPI-HD TX pipeline: buffers=%u", SPI_HD_TX_BATCH_BUFFERS);
    for (i = 0; i < SPI_HD_TX_POOL_SIZE; i++) {
        spi_hd_tx_slot_t *slot = &s_tx_pool[i];
        slot->buf = heap_caps_malloc(SPI_HD_FRAME_BUF_SIZE, MALLOC_CAP_8BIT);
        if (!slot->buf)
            goto fail;
        if (xQueueSend(s_tx_free_slots, &slot, 0) != pdTRUE)
            goto fail;
    }
#endif
    spi_hd_set_running(true);
    if (xTaskCreate(spi_hd_rx_task, "spi_hd_rx",
                    TASK_DEFAULT_STACK_SIZE, NULL, TASK_DEFAULT_PRIO,
                    &spi_hd_rx_task_handle) != pdTRUE)
        goto fail_running;
    if (xTaskCreate(spi_hd_tx_done_task, "spi_hd_tx_done",
                    TASK_DEFAULT_STACK_SIZE, NULL, TASK_DEFAULT_PRIO,
                    &spi_hd_tx_done_task_handle) != pdTRUE)
        goto fail_running;

#ifdef CONFIG_ESP_SPI_HD_TX_BATCH
    if (xTaskCreate(spi_hd_tx_batch_task, "spi_hd_tx_batch",
                    TASK_DEFAULT_STACK_SIZE, NULL, TASK_DEFAULT_PRIO,
                    &s_tx_batch_task_handle) != pdTRUE)
        goto fail_running;
    value = SPI_HD_TX_BATCH_SIZE * SPI_HD_TX_INFLIGHT_MAX;
#else
    value = SPI_HD_FRAME_BUF_SIZE;
#endif
    spi_hd_write_reg32(SPIHD_REG_MAX_TX_BUF_LEN, value);

    /* RX descriptors are segment-sized; advertise the true WRDMA limit. */
    value = SPI_HD_RX_SEGMENT_SIZE;
    spi_hd_write_reg32(SPIHD_REG_MAX_RX_BUF_LEN, value);

    do {
        session_lo = esp_random();
        session_hi = esp_random();
    } while (!(session_lo | session_hi));
    spi_hd_write_reg32(SPIHD_REG_SESSION_LO, session_lo);
    spi_hd_write_reg32(SPIHD_REG_SESSION_HI, session_hi);
    spi_hd_write_reg32(SPIHD_REG_RX_CREDIT_WINDOW, SPI_HD_RX_QUEUE_SIZE);
    spi_hd_write_reg32(SPIHD_REG_CONTROL_MAX_HZ,
                       (CONFIG_ESP_SPI_HD_CONTROL_MAX_CLK_MHZ < CONFIG_ESP_SPI_HD_MAX_CLK_MHZ ?
                        CONFIG_ESP_SPI_HD_CONTROL_MAX_CLK_MHZ : CONFIG_ESP_SPI_HD_MAX_CLK_MHZ) * 1000000U);
    spi_hd_write_reg32(SPIHD_REG_PAYLOAD_MAX_HZ,
                       CONFIG_ESP_SPI_HD_MAX_CLK_MHZ * 1000000U);
    value = SPIHD_STATE_SLAVE_READY | SPIHD_CAP_PROTOCOL_V3;
#ifdef CONFIG_ESP_SPI_HD_DMA_APPEND
    value |= SPIHD_CAP_DMA_APPEND;
#endif
#ifdef CONFIG_ESP_SPI_HD_TX_LEN_MIRROR
    spi_hd_write_reg32(SPIHD_REG_TX_LEN_MIRROR + 4, 0);
    spi_hd_write_reg32(SPIHD_REG_TX_LEN_MIRROR, 0);
    value |= SPIHD_CAP_TX_LEN_MIRROR;
#endif
    spi_hd_write_reg32(SPIHD_REG_SLAVE_READY, value);

    return &if_handle_g;

fail_running:
fail:
    ESP_LOGE(TAG, "SPI-HD initialization failed");
    spi_hd_cleanup_resources();
    return NULL;
}

static esp_err_t esp_spi_hd_reset(interface_handle_t *handle)
{
    (void)handle;
    return ESP_OK;
}

static void esp_spi_hd_deinit(interface_handle_t *handle)
{
    (void)handle;
    spi_hd_cleanup_resources();
}

interface_context_t *interface_insert_driver(int (*event_handler)(uint8_t val))
{
    ESP_LOGI(TAG, "Using SPI half-duplex/QIO interface");
    memset(&context, 0, sizeof(context));
    context.type = SPI_HD;
    context.if_ops = &if_ops;
    context.event_handler = event_handler;
    return &context;
}

int interface_remove_driver(void)
{
    memset(&context, 0, sizeof(context));
    return 0;
}

static esp_err_t spi_hd_send_bootup_event(uint32_t cap, uint32_t ext_cap)
{
    struct esp_payload_header *header;
    struct esp_internal_bootup_event *event;
    struct fw_data *fw_p;
    uint8_t *frame;
    uint8_t *pos;
    uint16_t len = 0;
    size_t total_len;
    uint32_t cap_le;
    uint32_t rx_buf_sz;
    esp_err_t ret;

    frame = heap_caps_calloc(1, SPI_HD_FRAME_BUF_SIZE, MALLOC_CAP_DMA);
    if (!frame)
        return ESP_ERR_NO_MEM;

    header = (struct esp_payload_header *)frame;
    header->if_type = ESP_INTERNAL_IF;
    header->if_num = 0;
    header->packet_type = PACKET_TYPE_EVENT;
    header->offset = htole16(sizeof(*header));

    event = (struct esp_internal_bootup_event *)(frame + sizeof(*header));
    event->header.event_code = ESP_INTERNAL_BOOTUP_EVENT;
    event->header.status = 0;
    pos = event->data;

    *pos++ = ESP_BOOTUP_FIRMWARE_CHIP_ID; len++;
    *pos++ = LENGTH_1_BYTE; len++;
    *pos++ = CONFIG_IDF_FIRMWARE_CHIP_ID; len++;

    *pos++ = ESP_BOOTUP_SPI_CLK_MHZ; len++;
    *pos++ = LENGTH_1_BYTE; len++;
    *pos++ = CONFIG_ESP_SPI_HD_MAX_CLK_MHZ; len++;

    *pos++ = ESP_BOOTUP_CAPABILITY; len++;
    *pos++ = 4; len++;
    cap_le = htole32(cap);
    memcpy(pos, &cap_le, sizeof(cap_le));
    pos += sizeof(cap_le);
    len += sizeof(cap_le);

    /* Keep SPI-HD boot capabilities compatible with SPI/SDIO/USB.
     * Extended capabilities announce available RCP/radio services.
     * The per-session RCP nonce is sent only after activation.
     */
    if (ext_cap) {
        uint32_t ext_cap_le = htole32(ext_cap);

        *pos++ = ESP_BOOTUP_EXT_CAPABILITY; len++;
        *pos++ = sizeof(ext_cap_le); len++;
        memcpy(pos, &ext_cap_le, sizeof(ext_cap_le));
        pos += sizeof(ext_cap_le);
        len += sizeof(ext_cap_le);
    }

    *pos++ = ESP_BOOTUP_RX_BUF_SIZE; len++;
    *pos++ = 4; len++;
    rx_buf_sz = htole32(SPI_HD_FRAME_BUF_SIZE);
    memcpy(pos, &rx_buf_sz, sizeof(rx_buf_sz));
    pos += sizeof(rx_buf_sz);
    len += sizeof(rx_buf_sz);

    *pos++ = ESP_BOOTUP_FW_DATA; len++;
    *pos++ = sizeof(struct fw_data); len++;
    fw_p = (struct fw_data *)pos;
    fw_p->last_reset_reason = htole32(rtc_get_reset_reason(0));
    strlcpy(fw_p->version.project_name, PROJECT_NAME,
            sizeof(fw_p->version.project_name));
    fw_p->version.major1 = PROJECT_VERSION_MAJOR_1;
    fw_p->version.major2 = PROJECT_VERSION_MAJOR_2;
    fw_p->version.minor = PROJECT_VERSION_MINOR;
    fw_p->version.revision_patch_1 = PROJECT_REVISION_PATCH_1;
    fw_p->version.revision_patch_2 = PROJECT_REVISION_PATCH_2;
    len += sizeof(*fw_p);

    event->len = len;
    event->header.len = htole16(len + 1);
    header->len = htole16(len + sizeof(struct esp_internal_bootup_event));

    total_len = sizeof(*header) + le16toh(header->len);
#if CONFIG_ESP_SPI_HD_CHECKSUM
    header->checksum = htole16(compute_checksum(frame, total_len));
#endif

    ret = spi_hd_queue_frame(frame, SPIHD_WIRE_SPAN(total_len), false, 0);
    return ret;
}

esp_err_t send_bootup_event_to_host(uint32_t cap, uint32_t ext_cap)
{
    esp_err_t ret;
    if (!spi_hd_api_enter())
        return ESP_ERR_INVALID_STATE;
    ret = spi_hd_send_bootup_event(cap, ext_cap);
    spi_hd_api_leave();
    return ret;
}
