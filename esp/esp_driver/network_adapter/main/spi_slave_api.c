// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2021 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sdkconfig.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <rom/rtc.h>
#include "esp.h"
#include "esp_log.h"
#include "interface.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "endian.h"
#include "freertos/task.h"
#include "stats.h"
#include "soc/gpio_reg.h"
#include "esp_fw_version.h"
#ifdef CONFIG_ESP_HOSTED_RCP
#include "esp_rcp.h"
#endif

// de-assert HS signal on CS, instead of at end of transaction
#if defined(CONFIG_ESP_SPI_DEASSERT_HS_ON_CS)
#define HS_DEASSERT_ON_CS (1)
#else
#define HS_DEASSERT_ON_CS (0)
#endif

static const char TAG[] = "FW_SPI";
#define SPI_BITS_PER_WORD           8
#define SPI_MODE_0              0
#define SPI_MODE_1              1
#define SPI_MODE_2              2
#define SPI_MODE_3              3

#if CONFIG_ESP_SPI_MODE < SPI_MODE_0 || CONFIG_ESP_SPI_MODE > SPI_MODE_3
#error "CONFIG_ESP_SPI_MODE must be in range 0..3"
#endif

#define SPI_DMA_ALIGNMENT_BYTES 4
#define SPI_DMA_ALIGNMENT_MASK  (SPI_DMA_ALIGNMENT_BYTES-1)
#define IS_SPI_DMA_ALIGNED(VAL) (!(((uintptr_t)(VAL)) & SPI_DMA_ALIGNMENT_MASK))
#define MAKE_SPI_DMA_ALIGNED(VAL)  (VAL += SPI_DMA_ALIGNMENT_BYTES - \
                ((VAL)& SPI_DMA_ALIGNMENT_MASK))

/* Chipset specific configurations */
#ifdef CONFIG_IDF_TARGET_ESP32

#if (CONFIG_ESP_SPI_CONTROLLER == 3)
#define ESP_SPI_CONTROLLER  2
#define GPIO_MOSI       23
#define GPIO_MISO       19
#define GPIO_SCLK       18
#define GPIO_CS         5
#elif (CONFIG_ESP_SPI_CONTROLLER == 2)
#define ESP_SPI_CONTROLLER  1
#define GPIO_MISO       12
#define GPIO_MOSI       13
#define GPIO_SCLK       14
#define GPIO_CS         15
#else
#error "Please choose correct SPI controller"
#endif

#define DMA_CHAN            ESP_SPI_CONTROLLER

#define SPI_CLK_MHZ         10

#elif defined CONFIG_IDF_TARGET_ESP32S2

#define ESP_SPI_CONTROLLER      1
#define GPIO_MOSI           11
#define GPIO_MISO           13
#define GPIO_SCLK           12
#define GPIO_CS             10
#define DMA_CHAN            ESP_SPI_CONTROLLER

#define SPI_CLK_MHZ         30

#elif defined CONFIG_IDF_TARGET_ESP32C3

#define ESP_SPI_CONTROLLER      1
#define GPIO_MOSI           7
#define GPIO_MISO           2
#define GPIO_SCLK           6
#define GPIO_CS             10
#define DMA_CHAN            SPI_DMA_CH_AUTO

#define SPI_CLK_MHZ         30

#elif defined CONFIG_IDF_TARGET_ESP32S3

#define ESP_SPI_CONTROLLER      1
#define GPIO_MOSI           11
#define GPIO_MISO           13
#define GPIO_SCLK           12
#define GPIO_CS             10
#define DMA_CHAN            SPI_DMA_CH_AUTO

#define SPI_CLK_MHZ         30

#elif defined CONFIG_IDF_TARGET_ESP32C2

#define ESP_SPI_CONTROLLER      1
#define GPIO_MOSI           7
#define GPIO_MISO           2
#define GPIO_SCLK           6
#define GPIO_CS             10
#define DMA_CHAN            SPI_DMA_CH_AUTO

#define SPI_CLK_MHZ         30

#elif defined CONFIG_IDF_TARGET_ESP32C5

#define ESP_SPI_CONTROLLER     1
#define GPIO_MOSI              7
#define GPIO_MISO              2
#define GPIO_SCLK              6
#define GPIO_CS                10
#define DMA_CHAN               SPI_DMA_CH_AUTO

#define SPI_CLK_MHZ            30

#elif defined CONFIG_IDF_TARGET_ESP32C6

#define ESP_SPI_CONTROLLER      1
#define GPIO_MOSI           7
#define GPIO_MISO           2
#define GPIO_SCLK           6
#define GPIO_CS             10
#define DMA_CHAN            SPI_DMA_CH_AUTO

#define SPI_CLK_MHZ         26

#elif defined CONFIG_IDF_TARGET_ESP32C61

#define ESP_SPI_CONTROLLER      1
#define GPIO_MOSI           7
#define GPIO_MISO           2
#define GPIO_SCLK           6
#define GPIO_CS             8
#define DMA_CHAN            SPI_DMA_CH_AUTO

#define SPI_CLK_MHZ         26

#elif defined CONFIG_IDF_TARGET_ESP32S31

/* GPSPI2 (FSPI) IO_MUX quad pin set 1: CLK=20, MOSI=21, MISO=22, CS=23 */
#define ESP_SPI_CONTROLLER      1
#define GPIO_MOSI           21
#define GPIO_MISO           22
#define GPIO_SCLK           20
#define GPIO_CS             23
#define DMA_CHAN            SPI_DMA_CH_AUTO

#define SPI_CLK_MHZ         26

#endif
/* Max SPI slave CLK in IO_MUX tested in IDF:
 * ESP32: 10MHz
 * ESP32-C2/C3/S2/S3: 40MHz
 * ESP32-C6: 26MHz
 */

#define SPI_QUEUE_SIZE          3

#define SPI_TX_QUEUE_SIZE       CONFIG_ESP_SPI_TX_Q_SIZE
#define SPI_RX_QUEUE_SIZE       CONFIG_ESP_SPI_RX_Q_SIZE

static interface_context_t context;
static interface_handle_t if_handle_g;
static uint8_t gpio_handshake;
static uint8_t gpio_data_ready;
static QueueHandle_t spi_rx_queue[MAX_PRIORITY_QUEUES] = {NULL};
static QueueHandle_t spi_tx_queue[MAX_PRIORITY_QUEUES] = {NULL};

#define SPI_SLOT_NUM            3
#define SPI_RX_POOL_SIZE        (SPI_SLOT_NUM + SPI_RX_QUEUE_SIZE + 2)

typedef struct {
    spi_slave_transaction_t trans;
    uint8_t *dma_tx;
    bool in_use;
    bool has_tx;
    bool is_raw_tp;
    bool reboot_ack;
    uint32_t raw_tp_run_id;
} spi_slot_t;

static spi_slot_t s_slots[SPI_SLOT_NUM];
static QueueHandle_t s_rx_buf_pool = NULL;
static uint8_t *s_emergency_rx_buf = NULL;
static bool s_emergency_rx_in_use = false;
static TaskHandle_t s_spi_post_process_task_hdl = NULL;

#if HS_DEASSERT_ON_CS
static SemaphoreHandle_t wait_cs_deassert_sem;
#endif

static interface_handle_t * esp_spi_init(void);
static int32_t esp_spi_write(interface_handle_t *handle,
                             interface_buffer_handle_t *buf_handle);
static int esp_spi_read(interface_handle_t *if_handle, interface_buffer_handle_t * buf_handle);
static esp_err_t esp_spi_reset(interface_handle_t *handle);
static void esp_spi_deinit(interface_handle_t *handle);
static void esp_spi_read_done(void *handle);
static void queue_next_transaction(void);

static esp_err_t spi_validate_output_gpio(int gpio, const char *name)
{
    if (gpio < 0 || gpio >= GPIO_NUM_MAX ||
#if !defined(GPIO_OUT1_W1TS_REG) || !defined(GPIO_OUT1_W1TC_REG)
        gpio >= 32 ||
#else
        gpio >= 64 ||
#endif
        !GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        ESP_LOGE(TAG, "Invalid SPI %s GPIO %d for output", name, gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (gpio == GPIO_MOSI || gpio == GPIO_MISO ||
        gpio == GPIO_SCLK || gpio == GPIO_CS) {
        ESP_LOGE(TAG, "SPI %s GPIO %d collides with SPI bus pin", name, gpio);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* Caller must validate gpio via spi_validate_output_gpio() before calling. */
static inline void IRAM_ATTR spi_gpio_set_high(uint8_t gpio)
{
#ifdef GPIO_OUT1_W1TS_REG
    if (gpio >= 32) {
        WRITE_PERI_REG(GPIO_OUT1_W1TS_REG, (1U << (gpio - 32)));
        return;
    }
#endif
    WRITE_PERI_REG(GPIO_OUT_W1TS_REG, (1U << gpio));
}

/* Caller must validate gpio via spi_validate_output_gpio() before calling. */
static inline void IRAM_ATTR spi_gpio_set_low(uint8_t gpio)
{
#ifdef GPIO_OUT1_W1TC_REG
    if (gpio >= 32) {
        WRITE_PERI_REG(GPIO_OUT1_W1TC_REG, (1U << (gpio - 32)));
        return;
    }
#endif
    WRITE_PERI_REG(GPIO_OUT_W1TC_REG, (1U << gpio));
}

if_ops_t if_ops = {
    .init = esp_spi_init,
    .write = esp_spi_write,
    .read = esp_spi_read,
    .reset = esp_spi_reset,
    .deinit = esp_spi_deinit,
};

interface_context_t *interface_insert_driver(int (*event_handler)(uint8_t val))
{
    ESP_LOGI(TAG, "Using SPI interface");
    memset(&context, 0, sizeof(context));

    context.type = SPI;
    context.if_ops = &if_ops;
    context.event_handler = event_handler;

    return &context;
}

int interface_remove_driver()
{
    memset(&context, 0, sizeof(context));
    return 0;
}

#if HS_DEASSERT_ON_CS
static void IRAM_ATTR gpio_disable_hs_isr_handler(void* arg)
{
    int level = gpio_get_level(GPIO_CS);
    if (level == 0) {
        /* CS is asserted, disable HS */
        spi_gpio_set_low(gpio_handshake);
    } else {
        /* Last transaction complete, populate next one */
        if (wait_cs_deassert_sem)
            xSemaphoreGive(wait_cs_deassert_sem);
    }
}

static void register_hs_disable_pin(uint32_t gpio_num)
{
    if (gpio_num != -1) {
        gpio_reset_pin(gpio_num);

        gpio_config_t slave_disable_hs_pin_conf={
            .intr_type=GPIO_INTR_DISABLE,
            .mode=GPIO_MODE_INPUT,
            .pin_bit_mask=(1ULL<<gpio_num)
        };
        slave_disable_hs_pin_conf.pull_up_en = 1;
        gpio_config(&slave_disable_hs_pin_conf);
        gpio_set_intr_type(gpio_num, GPIO_INTR_ANYEDGE);
        gpio_install_isr_service(0);
        gpio_isr_handler_add(gpio_num, gpio_disable_hs_isr_handler, NULL);
    }
}
#endif

esp_err_t send_bootup_event_to_host(uint32_t cap, uint32_t ext_cap)
{
    struct esp_payload_header *header = NULL;
    struct esp_internal_bootup_event *event = NULL;
    struct fw_data * fw_p = NULL;
    interface_buffer_handle_t buf_handle = {0};
    uint8_t * pos = NULL;
    uint16_t len = 0;

    memset(&buf_handle, 0, sizeof(buf_handle));

    buf_handle.payload = heap_caps_calloc(1, ESP_BOOTUP_EVENT_BUF_SIZE, MALLOC_CAP_DMA);
    if (!buf_handle.payload) {
        ESP_LOGE(TAG,
                 "Boot-event DMA allocation failed bytes=%u free_dma=%u largest_dma=%u",
                 ESP_BOOTUP_EVENT_BUF_SIZE,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        return ESP_ERR_NO_MEM;
    }

    buf_handle.priv_buffer_handle = buf_handle.payload;
    buf_handle.free_buf_handle = heap_caps_free;

    header = (struct esp_payload_header *) buf_handle.payload;

    header->if_type = ESP_INTERNAL_IF;
    header->if_num = 0;
    header->packet_type = PACKET_TYPE_EVENT;
    header->offset = htole16(sizeof(struct esp_payload_header));

    event = (struct esp_internal_bootup_event*)(buf_handle.payload + sizeof(struct esp_payload_header));

    event->header.event_code = ESP_INTERNAL_BOOTUP_EVENT;
    event->header.status = 0;

    pos = event->data;

    /* TLVs start */

    /* TLV - Board type */
    *pos = ESP_BOOTUP_FIRMWARE_CHIP_ID;   pos++; len++;
    *pos = LENGTH_1_BYTE;                 pos++; len++;
    *pos = CONFIG_IDF_FIRMWARE_CHIP_ID;   pos++; len++;

    /* TLV - Peripheral clock in MHz */
    *pos = ESP_BOOTUP_SPI_CLK_MHZ;        pos++; len++;
    *pos = LENGTH_1_BYTE;                 pos++; len++;
    *pos = SPI_CLK_MHZ;                   pos++; len++;

    /* TLV - Capability */
    *pos = ESP_BOOTUP_CAPABILITY;         pos++; len++;
    *pos = 4;                             pos++; len++;
    uint32_t cap_le = htole32(cap);
    memcpy(pos, &cap_le, sizeof(cap_le));
    pos += sizeof(cap_le);                len += sizeof(cap_le);

    /* TLV - Extended capabilities and RCP firmware incarnation. */
    if (ext_cap) {
        uint32_t ext_cap_le = htole32(ext_cap);

        *pos = ESP_BOOTUP_EXT_CAPABILITY; pos++; len++;
        *pos = sizeof(ext_cap_le);         pos++; len++;
        memcpy(pos, &ext_cap_le, sizeof(ext_cap_le));
        pos += sizeof(ext_cap_le);          len += sizeof(ext_cap_le);
    }

    /* RCP nonce is published only by CMD_SET_RADIO_SERVICE after activation. */

    esp_hosted_add_boot_mac_tlv(&pos, &len);

    /* TLV - Slave RX Buffer Size */
    *pos = ESP_BOOTUP_RX_BUF_SIZE;        pos++; len++;
    *pos = 4;                             pos++; len++;
    uint32_t rx_buf_sz = htole32(RX_BUF_SIZE);
    memcpy(pos, &rx_buf_sz, sizeof(rx_buf_sz));
    pos += sizeof(rx_buf_sz);             len += sizeof(rx_buf_sz);

    /* TLV - FW data */
    *pos = ESP_BOOTUP_FW_DATA;            pos++; len++;
    *pos = sizeof(struct fw_data);        pos++; len++;
    fw_p = (struct fw_data *) pos;
    /* core0 sufficient now */
    fw_p->last_reset_reason = htole32(rtc_get_reset_reason(0));
    memcpy(fw_p->version.project_name, PROJECT_NAME, strlen(PROJECT_NAME));
    fw_p->version.project_name[strlen(PROJECT_NAME)] = '\0';
    fw_p->version.major1 = PROJECT_VERSION_MAJOR_1;
    fw_p->version.major2 = PROJECT_VERSION_MAJOR_2;
    fw_p->version.minor  = PROJECT_VERSION_MINOR;
    fw_p->version.revision_patch_1  = PROJECT_REVISION_PATCH_1;
    fw_p->version.revision_patch_2  = PROJECT_REVISION_PATCH_2;
    pos += sizeof(struct fw_data);
    len += sizeof(struct fw_data);

    /* TLVs end */
    event->len = len;
    buf_handle.payload_len = len + sizeof(struct esp_internal_bootup_event) + sizeof(struct esp_payload_header);
    /*print_reset_reason(event->last_reset_reason);*/

    /* payload len = Event len + sizeof(event len) */
    len += 1;
    event->header.len = htole16(len);

    header->len = htole16(buf_handle.payload_len - sizeof(struct esp_payload_header));

    if (buf_handle.payload_len > ESP_BOOTUP_EVENT_BUF_SIZE) {
        ESP_LOGE(TAG, "Boot event too large len=%u max=%u",
                 buf_handle.payload_len, ESP_BOOTUP_EVENT_BUF_SIZE);
        heap_caps_free(buf_handle.payload);
        return ESP_ERR_INVALID_SIZE;
    }

#if CONFIG_ESP_SPI_CHECKSUM
    header->checksum = htole16(compute_checksum(buf_handle.payload, buf_handle.payload_len));
#endif

    xQueueSend(spi_tx_queue[PRIO_Q_HIGH], &buf_handle, portMAX_DELAY);

    /* Arm the initial SPI slot; because no prior dummy slot is mounted at
     * startup, spi_post_setup_cb() asserts both data_ready and handshake
     * once the boot frame's DMA descriptor is loaded. */
    queue_next_transaction();

    return ESP_OK;
}

/*
 * Two-stage SPI E2H readiness contract:
 * 1. Slot setup (spi_post_setup_cb):
 *    - HANDSHAKE is asserted only after spi_intr() has loaded the active
 *      slot's DMA descriptors and armed the slave peripheral. The host
 *      never clocks a transfer unless HANDSHAKE is high.
 *    - When the armed slot itself carries an E2H payload (slot->has_tx,
 *      including the initial boot-up event in slot 0), DATA_READY is raised
 *      here together with HANDSHAKE so the host cannot observe
 *      (HANDSHAKE && DATA_READY) before the TX DMA descriptor is armed.
 * 2. Runtime wake-up behind an armed idle dummy slot (esp_spi_write):
 *    - When the slave is idle (spi_tx_queue[] empty), queue_next_transaction()
 *      arms a dummy slot (0xF header, slot->has_tx == false) with HANDSHAKE
 *      high and DATA_READY low so the host may still initiate H2E transfers.
 *    - ESP-IDF's spi_slave driver cannot replace an already-armed dummy DMA
 *      descriptor in place without a master clock cycle. When esp_spi_write()
 *      enqueues an E2H frame while an idle dummy slot is armed, it raises
 *      DATA_READY ("E2H TX pending") to wake the host. The host clocks out
 *      and discards the armed dummy slot (spi_is_dummy_rx()), whose completion
 *      triggers queue_next_transaction() to arm the queued E2H frame in the
 *      next slot.
 */
static bool spi_tx_queues_pending(void)
{
    return uxQueueMessagesWaiting(spi_tx_queue[PRIO_Q_HIGH]) ||
           uxQueueMessagesWaiting(spi_tx_queue[PRIO_Q_MID]) ||
           uxQueueMessagesWaiting(spi_tx_queue[PRIO_Q_LOW]);
}

/*
 * Synchronize DATA_READY when arming an idle dummy slot (slot->has_tx == false).
 *
 * Clear DATA_READY first, issue a full barrier, and recheck spi_tx_queue[].
 * Because esp_spi_write() enqueues into spi_tx_queue[] before asserting
 * DATA_READY, this store-before-load ordering guarantees that a concurrent
 * producer cannot have its DATA_READY wakeup clobbered while an idle dummy
 * slot is being prepared.
 */
static bool spi_sync_dummy_slot_data_ready(void)
{
    spi_gpio_set_low(gpio_data_ready);
    __sync_synchronize();
    if (spi_tx_queues_pending()) {
        spi_gpio_set_high(gpio_data_ready);
        return true;
    }
    return false;
}

static void IRAM_ATTR spi_post_setup_cb(spi_slave_transaction_t *trans)
{
    const spi_slot_t *slot = trans ? (const spi_slot_t *)trans->user : NULL;

    /* Assert data_ready (when this armed slot carries TX data) and handshake
     * only after spi_intr() has armed the SPI slave DMA peripheral. */
    if (slot && slot->has_tx) {
        spi_gpio_set_high(gpio_data_ready);
    }
    spi_gpio_set_high(gpio_handshake);
}

/* Invoked after transaction is sent/received.
 * Use this to set the handshake line low */
static void IRAM_ATTR spi_post_trans_cb(spi_slave_transaction_t *trans)
{
#if !HS_DEASSERT_ON_CS
    /* Clear handshake line */
    spi_gpio_set_low(gpio_handshake);
#endif
}

static bool get_next_tx_buffer(interface_buffer_handle_t *buf_handle)
{
    esp_err_t ret = ESP_OK;

    if (!buf_handle) {
        return false;
    }

    while (1) {
        memset(buf_handle, 0, sizeof(*buf_handle));

        /* Get buffer from SPI Tx queue */
        if (uxQueueMessagesWaiting(spi_tx_queue[PRIO_Q_HIGH])) {
            ret = xQueueReceive(spi_tx_queue[PRIO_Q_HIGH], buf_handle, 0);
        } else if (uxQueueMessagesWaiting(spi_tx_queue[PRIO_Q_MID])) {
            ret = xQueueReceive(spi_tx_queue[PRIO_Q_MID], buf_handle, 0);
        } else if (uxQueueMessagesWaiting(spi_tx_queue[PRIO_Q_LOW])) {
            ret = xQueueReceive(spi_tx_queue[PRIO_Q_LOW], buf_handle, 0);
        } else {
            ret = pdFALSE;
        }

        if (ret == pdTRUE && buf_handle->payload) {
            if (buf_handle->if_type == ESP_TEST_IF &&
                !debug_raw_tp_is_run_active(buf_handle->raw_tp_run_id)) {
                if (buf_handle->free_buf_handle && buf_handle->priv_buffer_handle) {
                    buf_handle->free_buf_handle(buf_handle->priv_buffer_handle);
                    buf_handle->priv_buffer_handle = NULL;
                }
                continue;
            }
            return true;
        }
        break;
    }

    return false;
}

/*
 * Linux clocks a full fixed-size SPI transaction to receive an advertised
 * E2H frame even when it has no H2E payload to send. In that case the host TX
 * buffer is deliberately all zeroes. This is transport idle, not a truncated
 * RCP frame. Accept only the exact all-zero transaction so any nonzero
 * malformed bytes remain fail-closed once an RCP session is active.
 */
static bool spi_is_host_idle_transaction(const uint8_t *buf)
{
    size_t i;

    if (!buf)
        return false;

    for (i = 0; i < RX_BUF_SIZE; i++) {
        if (buf[i] != 0)
            return false;
    }
    return true;
}

static void spi_fail_rcp_rx(const struct esp_payload_header *header,
                            const char *reason)
{
#ifdef CONFIG_ESP_HOSTED_RCP
    if (esp_hosted_rcp_fail_closed_required()) {
        ESP_LOGE(TAG, "RCP SPI H2E stream may be truncated: %s if=%u type=%u",
                 reason, header ? header->if_type : 0xff,
                 header ? header->packet_type : 0xff);
        esp_hosted_rcp_fatal_restart(reason);
    }
#else
    (void)header;
    (void)reason;
#endif
}

static int process_spi_rx(interface_buffer_handle_t *buf_handle)
{
    int ret = 0;
    struct esp_payload_header *header = NULL;
    uint16_t len = 0, offset = 0;
#if CONFIG_ESP_SPI_CHECKSUM
    uint16_t rx_checksum = 0, checksum = 0;
#endif

    /* Validate received buffer. Drop invalid buffer. */

    if (!buf_handle || !buf_handle->payload) {
        ESP_LOGE(TAG, "%s: Invalid params", __func__);
        return -1;
    }

    header = (struct esp_payload_header *) buf_handle->payload;
    if (header->if_type >= ESP_MAX_IF) {
        spi_fail_rcp_rx(header, "invalid SPI H2E interface type");
        return -1;
    }
    len = le16toh(header->len);
    offset = le16toh(header->offset);

    if (len == 0) {
        if (spi_is_host_idle_transaction((const uint8_t *)buf_handle->payload))
            return -1;
        spi_fail_rcp_rx(header, "zero SPI H2E payload");
        return -1;
    }
    if (len > RX_BUF_SIZE || !ESP_OFFSET_VALID(offset) ||
        offset > RX_BUF_SIZE || len > (RX_BUF_SIZE - offset)) {
        ESP_LOGE(TAG, "Drop invalid pkt: len=%d offset=%d", len, offset);
        spi_fail_rcp_rx(header, "invalid SPI H2E length/offset");
        return -1;
    }

#if CONFIG_ESP_SPI_CHECKSUM
    rx_checksum = le16toh(header->checksum);
    header->checksum = 0;

    if (len + offset > RX_BUF_SIZE) {
        ESP_LOGD(TAG, "total len too large: %d", len + offset);
        spi_fail_rcp_rx(header, "SPI H2E frame too large");
        return -1;
    }
    checksum = compute_checksum(buf_handle->payload, len + offset);

    if (checksum != rx_checksum) {
        ESP_LOGD(TAG, "checksum mismatch");
        spi_fail_rcp_rx(header, "SPI H2E checksum mismatch");
        return -1;
    }
#endif

    /* Buffer is valid */
    buf_handle->if_type = header->if_type;
    buf_handle->if_num = header->if_num;
    buf_handle->free_buf_handle = esp_spi_read_done;
    buf_handle->payload_len = le16toh(header->len) + offset;
    buf_handle->priv_buffer_handle = buf_handle->payload;

    if (header->if_type == ESP_INTERNAL_IF) {
        ret = xQueueSend(spi_rx_queue[PRIO_Q_HIGH], buf_handle, portMAX_DELAY);
    } else if (header->if_type == ESP_RCP_IF) {
        ret = xQueueSend(spi_rx_queue[PRIO_Q_MID], buf_handle,
                         pdMS_TO_TICKS(ESP_HOSTED_RCP_TX_TIMEOUT_MS));
    } else if (header->if_type == ESP_HCI_IF) {
        ret = xQueueSend(spi_rx_queue[PRIO_Q_MID], buf_handle, portMAX_DELAY);
    } else {
        ret = xQueueSend(spi_rx_queue[PRIO_Q_LOW], buf_handle, portMAX_DELAY);
    }

    if (ret != pdTRUE) {
        spi_fail_rcp_rx(header, "SPI H2E RX queue stalled");
        return -1;
    }

    return 0;
}

static inline void recycle_spi_rx_buf(uint8_t *rx_buf)
{
    if (!rx_buf) {
        return;
    }
    if (rx_buf == s_emergency_rx_buf) {
        s_emergency_rx_in_use = false;
    } else if (!s_rx_buf_pool || xQueueSend(s_rx_buf_pool, &rx_buf, 0) != pdTRUE) {
        free(rx_buf);
    }
}

static void queue_next_transaction(void)
{
    esp_err_t ret = ESP_OK;
    spi_slot_t *slot = NULL;
    interface_buffer_handle_t buf_handle = {0};
    uint8_t *rx_buf = NULL;
    bool has_tx = false;
    int i = 0;

    /* 1. Find an idle slot */
    for (i = 0; i < SPI_SLOT_NUM; i++) {
        if (!s_slots[i].in_use) {
            slot = &s_slots[i];
            break;
        }
    }
    if (!slot) {
        ESP_LOGE(TAG, "FATAL: No free SPI transaction slot, restarting\n");
        esp_restart();
        return;
    }

    /* 2. Acquire RX buffer: pool -> emergency buffer -> emergency allocation */
    if (s_rx_buf_pool && xQueueReceive(s_rx_buf_pool, &rx_buf, 0) == pdTRUE && rx_buf) {
        /* acquired from pool */
    } else if (!s_emergency_rx_in_use && s_emergency_rx_buf) {
        rx_buf = s_emergency_rx_buf;
        s_emergency_rx_in_use = true;
    } else {
        rx_buf = heap_caps_malloc(RX_BUF_SIZE, MALLOC_CAP_DMA);
        if (!rx_buf) {
            ESP_LOGE(TAG, "FATAL: No RX buffer available for SPI transaction, restarting\n");
            esp_restart();
            return;
        }
    }

    /* 3. Mark slot in use BEFORE dequeuing TX buffer, guaranteeing resources are held */
    slot->in_use = true;
    slot->has_tx = false;
    slot->is_raw_tp = false;
    slot->reboot_ack = false;
    slot->raw_tp_run_id = 0;

    /* 4. Dequeue next TX buffer if available */
    has_tx = get_next_tx_buffer(&buf_handle);
    slot->has_tx = has_tx;
    if (has_tx) {
        slot->is_raw_tp = (buf_handle.if_type == ESP_TEST_IF);
        slot->raw_tp_run_id = buf_handle.raw_tp_run_id;
        if (buf_handle.if_type == ESP_INTERNAL_IF &&
            buf_handle.payload_len >=
                sizeof(struct esp_payload_header) + sizeof(struct command_header)) {
            /* esp_spi_write() already placed the real wire header in
             * this queued payload; buf_handle.pkt_type is not populated
             * in the transport queue item. */
            const struct esp_payload_header *wire =
                (const struct esp_payload_header *)buf_handle.payload;
            uint16_t offset = le16toh(wire->offset);
            if (wire->if_type == ESP_INTERNAL_IF &&
                wire->packet_type == PACKET_TYPE_COMMAND_RESPONSE &&
                ESP_OFFSET_VALID(offset) &&
                offset <= buf_handle.payload_len &&
                sizeof(struct command_header) <=
                    buf_handle.payload_len - offset) {
                const struct command_header *cmd =
                    (const struct command_header *)(buf_handle.payload + offset);
                slot->reboot_ack = cmd->cmd_code == CMD_HOST_REBOOT &&
                                   cmd->cmd_status == CMD_RESPONSE_SUCCESS;
            }
        }

        memcpy(slot->dma_tx, buf_handle.payload, buf_handle.payload_len);
        if (buf_handle.payload_len < RX_BUF_SIZE) {
            memset(slot->dma_tx + buf_handle.payload_len, 0, RX_BUF_SIZE - buf_handle.payload_len);
        }
        if (buf_handle.free_buf_handle) {
            buf_handle.free_buf_handle(buf_handle.priv_buffer_handle);
            buf_handle.free_buf_handle = NULL;
        }
    } else {
        /* No pending frame: prepare dummy header */
        memset(slot->dma_tx, 0, RX_BUF_SIZE);
        struct esp_payload_header *dummy_hdr = (struct esp_payload_header *)slot->dma_tx;
        dummy_hdr->if_type = 0xF;
        dummy_hdr->if_num  = 0xF;
        dummy_hdr->len     = 0;

        /* Clear data_ready before rechecking TX queues so a concurrent
         * esp_spi_write() producer cannot lose its wakeup. */
        (void)spi_sync_dummy_slot_data_ready();
    }

    /* 5. Set up transaction */
    memset(&slot->trans, 0, sizeof(spi_slave_transaction_t));
    memset(rx_buf, 0, RX_BUF_SIZE);
    slot->trans.rx_buffer = rx_buf;
    slot->trans.tx_buffer = slot->dma_tx;
    slot->trans.user = slot;
    slot->trans.length = RX_BUF_SIZE * SPI_BITS_PER_WORD;

    ret = spi_slave_queue_trans(ESP_SPI_CONTROLLER, &slot->trans, portMAX_DELAY);
    if (ret != ESP_OK) {
        /* Invariant: Once a real E2H state/control frame is dequeued, either that exact frame
         * becomes an armed SPI transaction or the firmware incarnation is invalidated.
         */
        ESP_LOGE(TAG, "FATAL: Failed to queue next SPI transfer (0x%x), restarting\n", ret);
        esp_restart();
        return;
    }
}

static void spi_transaction_post_process_task(void* pvParameters)
{
    esp_err_t ret = ESP_OK;
    spi_slave_transaction_t *spi_trans = NULL;
    spi_slot_t *slot = NULL;
    interface_buffer_handle_t rx_buf_handle = {0};

    for (;;) {
        bool reboot_after_ack = false;

        memset(&rx_buf_handle, 0, sizeof(rx_buf_handle));

        /* Await transmission result, after any kind of transmission a new packet
         * (dummy or real) must normally be placed in SPI slave.
         */
        ret = spi_slave_get_trans_result(ESP_SPI_CONTROLLER, &spi_trans,
                                         portMAX_DELAY);

#if HS_DEASSERT_ON_CS
        /* Wait until CS has been deasserted before we queue a new transaction.
         *
         * Some MCUs delay deasserting CS at the end of a transaction.
         * If we queue a new transaction without waiting for CS to deassert,
         * the slave SPI can start (since CS is still asserted), and data is lost
         * as host is not expecting any data.
         */
        if (wait_cs_deassert_sem) {
            xSemaphoreTake(wait_cs_deassert_sem, portMAX_DELAY);
        }
#endif

        if (spi_trans && spi_trans->user) {
            slot = (spi_slot_t *)spi_trans->user;
            reboot_after_ack = slot->reboot_ack;
        } else {
            slot = NULL;
        }

        /* CMD35 is a reincarnation fence only after its ACK transaction
         * completed successfully. If that transaction failed, no reboot was
         * authorized and the slave must keep servicing SPI normally. */
        if (ret != ESP_OK || !reboot_after_ack)
            queue_next_transaction();

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi transmit error, ret : 0x%x\r\n", ret);
            if (slot) {
                if (slot->is_raw_tp) {
                    debug_raw_tp_tx_failed(1);
                }
                if (spi_trans->rx_buffer) {
                    recycle_spi_rx_buf((uint8_t *)spi_trans->rx_buffer);
                }
                slot->reboot_ack = false;
                slot->in_use = false;
            }
            continue;
        }

        if (!spi_trans) {
            ESP_LOGW(TAG, "spi_trans fetched NULL\n");
            continue;
        }

        if (slot && slot->is_raw_tp) {
            debug_raw_tp_tx_complete(1);
        }

        if (reboot_after_ack) {
            /* Ignore the host MOSI side of the ACK-clocking transaction. It
             * belongs to the old incarnation and must not cause new side effects
             * while a full firmware restart is already committed. */
            if (spi_trans->rx_buffer)
                recycle_spi_rx_buf((uint8_t *)spi_trans->rx_buffer);
            if (slot) {
                slot->reboot_ack = false;
                slot->in_use = false;
            }
            ESP_LOGW(TAG, "HOST_REBOOT ACK delivered over SPI; restarting firmware");
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
            continue;
        }

        if (spi_trans->rx_buffer) {
            rx_buf_handle.payload = spi_trans->rx_buffer;
            ret = process_spi_rx(&rx_buf_handle);

            /* free rx_buffer if process_spi_rx returns an error or dummy.
             * In success case it will be freed later via esp_spi_read_done */
            if (ret != ESP_OK) {
                recycle_spi_rx_buf((uint8_t *)spi_trans->rx_buffer);
            }
        }

        if (slot) {
            slot->reboot_ack = false;
            slot->in_use = false;
        }
    }
}

static interface_handle_t * esp_spi_init(void)
{
    esp_err_t ret = ESP_OK;
    uint8_t prio_q_idx = 0;

    /* Configuration for the SPI bus */
    spi_bus_config_t buscfg = {
        .mosi_io_num = GPIO_MOSI,
        .miso_io_num = GPIO_MISO,
        .sclk_io_num = GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RX_BUF_SIZE,
#if 0
        /*
         * Moving ESP32 SPI slave interrupts in flash, Keeping it in IRAM gives crash,
         * While performing flash erase operation.
         */
        .intr_flags = ESP_INTR_FLAG_IRAM
#endif
    };

    ESP_LOGI(TAG, "Using SPI MODE %d", CONFIG_ESP_SPI_MODE);
    /* Configuration for the SPI slave interface */
    spi_slave_interface_config_t slvcfg = {
        .mode = CONFIG_ESP_SPI_MODE,
        .spics_io_num = GPIO_CS,
        .queue_size = SPI_QUEUE_SIZE,
        .flags = 0,
        .post_setup_cb = spi_post_setup_cb,
        .post_trans_cb = spi_post_trans_cb
    };

    ESP_ERROR_CHECK(spi_validate_output_gpio(CONFIG_ESP_SPI_GPIO_HANDSHAKE, "handshake"));
    ESP_ERROR_CHECK(spi_validate_output_gpio(CONFIG_ESP_SPI_GPIO_DATA_READY, "data_ready"));
    if (CONFIG_ESP_SPI_GPIO_HANDSHAKE == CONFIG_ESP_SPI_GPIO_DATA_READY) {
        ESP_LOGE(TAG, "SPI handshake and data_ready cannot share GPIO %d",
                 CONFIG_ESP_SPI_GPIO_HANDSHAKE);
        ESP_ERROR_CHECK(ESP_ERR_INVALID_ARG);
    }
    gpio_handshake = (uint8_t)CONFIG_ESP_SPI_GPIO_HANDSHAKE;
    gpio_data_ready = (uint8_t)CONFIG_ESP_SPI_GPIO_DATA_READY;

    /* Configuration for the handshake line */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << gpio_handshake)
    };

    /* Configuration for data_ready line */
    gpio_config_t io_data_ready_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << gpio_data_ready)
    };

    /* Configure handshake and data_ready lines as output, initially low */
    spi_gpio_set_low(gpio_handshake);
    spi_gpio_set_low(gpio_data_ready);
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_config(&io_data_ready_conf));
    spi_gpio_set_low(gpio_handshake);
    spi_gpio_set_low(gpio_data_ready);

    /* Enable pull-ups on SPI lines
     * so that no rogue pulses when no master is connected
     */
    ESP_ERROR_CHECK(gpio_set_pull_mode(GPIO_MOSI, GPIO_PULLUP_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(GPIO_SCLK, GPIO_PULLUP_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(GPIO_CS, GPIO_PULLUP_ONLY));

    ESP_LOGI(TAG, "SPI Ctrl:%u mode: %u, GPIOs: MOSI: %u, MISO: %u, CS: %u, CLK: %u HS: %u DR: %u\n",
        ESP_SPI_CONTROLLER, slvcfg.mode,
        GPIO_MOSI, GPIO_MISO, GPIO_CS, GPIO_SCLK, gpio_handshake, gpio_data_ready);
    ESP_LOGI(TAG, "Hosted SPI queue size: Tx:%u Rx:%u", SPI_TX_QUEUE_SIZE, SPI_RX_QUEUE_SIZE);

#if HS_DEASSERT_ON_CS
    register_hs_disable_pin(GPIO_CS);
#endif

    /* Initialize SPI slave interface */
    ret = spi_slave_initialize(ESP_SPI_CONTROLLER, &buscfg, &slvcfg, DMA_CHAN);
    assert(ret == ESP_OK);

    memset(&if_handle_g, 0, sizeof(if_handle_g));
    if_handle_g.state = INIT;

#if HS_DEASSERT_ON_CS
    wait_cs_deassert_sem = xSemaphoreCreateBinary();
    assert(wait_cs_deassert_sem!= NULL);
    /* Clear the semaphore */
    xSemaphoreTake(wait_cs_deassert_sem, 0);
#endif

    for (prio_q_idx = 0; prio_q_idx < MAX_PRIORITY_QUEUES; prio_q_idx++) {
        spi_rx_queue[prio_q_idx] = xQueueCreate(SPI_RX_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
        assert(spi_rx_queue[prio_q_idx] != NULL);

        spi_tx_queue[prio_q_idx] = xQueueCreate(SPI_TX_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
        assert(spi_tx_queue[prio_q_idx] != NULL);
    }

    for (int i = 0; i < SPI_SLOT_NUM; i++) {
        s_slots[i].dma_tx = heap_caps_malloc(RX_BUF_SIZE, MALLOC_CAP_DMA);
        assert(s_slots[i].dma_tx != NULL);
        s_slots[i].in_use = false;
        s_slots[i].has_tx = false;
        s_slots[i].is_raw_tp = false;
        s_slots[i].raw_tp_run_id = 0;
    }

    s_emergency_rx_buf = heap_caps_malloc(RX_BUF_SIZE, MALLOC_CAP_DMA);
    assert(s_emergency_rx_buf != NULL);
    s_emergency_rx_in_use = false;

    s_rx_buf_pool = xQueueCreate(SPI_RX_POOL_SIZE, sizeof(uint8_t *));
    assert(s_rx_buf_pool != NULL);

    for (int i = 0; i < SPI_RX_POOL_SIZE; i++) {
        uint8_t *rx_buf = heap_caps_malloc(RX_BUF_SIZE, MALLOC_CAP_DMA);
        assert(rx_buf != NULL);
        BaseType_t qret = xQueueSend(s_rx_buf_pool, &rx_buf, 0);
        assert(qret == pdTRUE);
    }

    assert(xTaskCreate(spi_transaction_post_process_task, "spi_post_process_task",
                       TASK_DEFAULT_STACK_SIZE, NULL, TASK_DEFAULT_PRIO,
                       &s_spi_post_process_task_hdl) == pdTRUE);

    usleep(500);

    return &if_handle_g;
}

static int32_t esp_spi_write(interface_handle_t *handle, interface_buffer_handle_t *buf_handle)
{
    esp_err_t ret = ESP_OK;
    int32_t total_len = 0;
    uint16_t offset = 0;
    uint16_t wire_offset = 0;
    struct esp_payload_header *header = NULL;
    interface_buffer_handle_t tx_buf_handle = {0};

    if (!handle || !buf_handle) {
        ESP_LOGE(TAG, "Invalid arguments\n");
        return ESP_FAIL;
    }

    if (!buf_handle->payload_len || !buf_handle->payload) {
        ESP_LOGE(TAG, "Invalid arguments, len:%d\n", buf_handle->payload_len);
        return ESP_FAIL;
    }

    total_len = buf_handle->payload_len + sizeof(struct esp_payload_header);

    /* make the addresses dma aligned */
    if (!IS_SPI_DMA_ALIGNED(total_len)) {
        MAKE_SPI_DMA_ALIGNED(total_len);
    }

    if (total_len > RX_BUF_SIZE) {
        ESP_LOGE(TAG, "Max frame length exceeded %ld.. drop it\n", total_len);
        if (buf_handle->if_type == ESP_TEST_IF) {
            debug_raw_tp_tx_failed(1);
        }
        return ESP_FAIL;
    }

    memset(&tx_buf_handle, 0, sizeof(tx_buf_handle));

    tx_buf_handle.if_type = buf_handle->if_type;
    tx_buf_handle.if_num = buf_handle->if_num;
    tx_buf_handle.payload_len = total_len;
    tx_buf_handle.raw_tp_run_id = buf_handle->raw_tp_run_id;

    uint32_t align_padding = 0;
    offset = sizeof(struct esp_payload_header);
    if (buf_handle->if_type == ESP_RCP_IF) {
        if (buf_handle->payload_len > ESP_HOSTED_RCP_CHUNK_MAX ||
            ESP_HOSTED_RCP_TX_HEADROOM < sizeof(struct esp_payload_header)) {
            ESP_LOGE(TAG, "Invalid RCP TX buffer len=%u headroom=%u",
                     buf_handle->payload_len, ESP_HOSTED_RCP_TX_HEADROOM);
            return ESP_ERR_INVALID_ARG;
        }

        offset = ESP_HOSTED_RCP_TX_HEADROOM;
        /*
         * RCP payloads already reserve the complete Hosted headroom. Do not
         * add that headroom/header delta again as transport alignment padding.
         */
        align_padding = 0;
        tx_buf_handle.payload = buf_handle->payload - offset;
        if (!IS_SPI_DMA_ALIGNED(tx_buf_handle.payload)) {
            ESP_LOGE(TAG, "RCP TX pool buffer is not word-aligned");
            return ESP_ERR_INVALID_ARG;
        }

        tx_buf_handle.priv_buffer_handle = buf_handle->priv_buffer_handle;
        tx_buf_handle.free_buf_handle = buf_handle->free_buf_handle;
        buf_handle->priv_buffer_handle = NULL;
        buf_handle->free_buf_handle = NULL;
    } else if (IS_WIFI_DATA_PACKET(buf_handle)) {
        /* As Wi-Fi esf-buf has headroom of rx_ctrl before Wi-Fi data pointer, we can use that space to store packet header.
         * This way we do not need to alloc and memcpy again */
        uint32_t payload_addr = (uint32_t)buf_handle->payload;
        align_padding = (SPI_DMA_ALIGNMENT_BYTES - (payload_addr % SPI_DMA_ALIGNMENT_BYTES)) % SPI_DMA_ALIGNMENT_BYTES;
        tx_buf_handle.payload = (uint8_t *)buf_handle->payload - sizeof(struct esp_payload_header) - align_padding;

        tx_buf_handle.priv_buffer_handle = buf_handle->priv_buffer_handle;
        tx_buf_handle.free_buf_handle = buf_handle->free_buf_handle;

        buf_handle->priv_buffer_handle = NULL;
        buf_handle->free_buf_handle = NULL;
    } else {
        tx_buf_handle.payload = heap_caps_malloc(buf_handle->payload_len + offset, MALLOC_CAP_DMA);
        if (!tx_buf_handle.payload) {
            if (buf_handle->if_type == ESP_TEST_IF) {
                debug_raw_tp_tx_failed(1);
            }
            return ESP_ERR_NO_MEM;
        }

        memcpy(tx_buf_handle.payload + offset, buf_handle->payload, buf_handle->payload_len);

        tx_buf_handle.priv_buffer_handle = tx_buf_handle.payload;
        tx_buf_handle.free_buf_handle = heap_caps_free;
    }

    wire_offset = offset + align_padding;
    total_len = buf_handle->payload_len + wire_offset;
    tx_buf_handle.payload_len = total_len;
    header = (struct esp_payload_header *) tx_buf_handle.payload;

    memset(header, 0, wire_offset);

    /* Initialize header */
    header->if_type = buf_handle->if_type;
    header->if_num = buf_handle->if_num;
    header->len = htole16(buf_handle->payload_len);
    header->offset = htole16(wire_offset);
    header->flags = buf_handle->flag;
    header->packet_type = buf_handle->pkt_type;
    if (header->if_type == ESP_TEST_IF) {
        debug_raw_tp_set_seq(header, buf_handle->raw_tp_seq);
    }

#if CONFIG_ESP_SPI_CHECKSUM
    header->checksum = htole16(compute_checksum(tx_buf_handle.payload,
                                                total_len));
#else
    /* The privileged CMD_HOST_REBOOT (35) ACK is always checksummed.
     * With CONFIG_ESP_SPI_CHECKSUM=n ordinary SPI frames remain unchecked,
     * but bootstrap recovery must never accept a corrupted reboot ACK.
     * The host expects this checksum even before boot negotiation. */
    if (header->if_type == ESP_INTERNAL_IF &&
        header->packet_type == PACKET_TYPE_COMMAND_RESPONSE &&
        buf_handle->payload_len >= sizeof(struct command_header)) {
        const struct command_header *cmd =
            (const struct command_header *)(tx_buf_handle.payload + wire_offset);
        if (cmd->cmd_code == CMD_HOST_REBOOT)
            header->checksum = htole16(compute_checksum(tx_buf_handle.payload,
                                                        total_len));
    }
#endif

    if (header->if_type == ESP_INTERNAL_IF) {
        ret = xQueueSend(spi_tx_queue[PRIO_Q_HIGH], &tx_buf_handle, portMAX_DELAY);
    } else if (header->if_type == ESP_RCP_IF) {
        /*
         * The generic Hosted queue is bounded, but SPI has this second
         * transport-specific queue. Do not let a stalled host pin the
         * OpenThread/NCP send path here indefinitely.
         */
        ret = xQueueSend(spi_tx_queue[PRIO_Q_MID], &tx_buf_handle,
                         pdMS_TO_TICKS(ESP_HOSTED_RCP_TX_TIMEOUT_MS));
    } else if (header->if_type == ESP_HCI_IF) {
        ret = xQueueSend(spi_tx_queue[PRIO_Q_MID], &tx_buf_handle, portMAX_DELAY);
    } else {
        ret = xQueueSend(spi_tx_queue[PRIO_Q_LOW], &tx_buf_handle, portMAX_DELAY);
    }

    if (ret != pdTRUE) {
        if (tx_buf_handle.payload) {
            if (tx_buf_handle.free_buf_handle && tx_buf_handle.priv_buffer_handle) {
                tx_buf_handle.free_buf_handle(tx_buf_handle.priv_buffer_handle);
            }
        }
        if (buf_handle->if_type == ESP_TEST_IF) {
            debug_raw_tp_tx_failed(1);
        }
        if (buf_handle->if_type == ESP_RCP_IF) {
            ESP_LOGE(TAG, "RCP SPI TX queue stalled for %u ms",
                     ESP_HOSTED_RCP_TX_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
        return ESP_FAIL;
    }

    /* Signal E2H TX pending on data_ready. If an idle dummy slot (0xF header,
     * slot->has_tx == false) is currently armed with HANDSHAKE high, this wakes
     * the host to clock and discard that dummy slot so queue_next_transaction()
     * can arm this queued frame in the next slot (see spi_post_setup_cb()). */
    spi_gpio_set_high(gpio_data_ready);

    return buf_handle->payload_len;
}

static void IRAM_ATTR esp_spi_read_done(void *handle)
{
    if (handle) {
        recycle_spi_rx_buf((uint8_t *)handle);
    }
}

static int esp_spi_read(interface_handle_t *if_handle, interface_buffer_handle_t *buf_handle)
{
    esp_err_t ret = ESP_OK;

    if (!if_handle) {
        ESP_LOGE(TAG, "Invalid arguments to esp_spi_read\n");
        return ESP_FAIL;
    }

    while (1) {
        if (uxQueueMessagesWaiting(spi_rx_queue[PRIO_Q_HIGH])) {
            ret = xQueueReceive(spi_rx_queue[PRIO_Q_HIGH], buf_handle, portMAX_DELAY);
            break;
        } else if (uxQueueMessagesWaiting(spi_rx_queue[PRIO_Q_MID])) {
            ret = xQueueReceive(spi_rx_queue[PRIO_Q_MID], buf_handle, portMAX_DELAY);
            break;
        } else if (uxQueueMessagesWaiting(spi_rx_queue[PRIO_Q_LOW])) {
            ret = xQueueReceive(spi_rx_queue[PRIO_Q_LOW], buf_handle, portMAX_DELAY);
            break;
        } else {
            vTaskDelay(1);
        }
    }

    if (ret != pdTRUE) {
        return ESP_FAIL;
    }
    return buf_handle->payload_len;
}

static esp_err_t esp_spi_reset(interface_handle_t *handle)
{
    esp_err_t ret = ESP_OK;
    ret = spi_slave_free(ESP_SPI_CONTROLLER);
    if (ESP_OK != ret) {
        ESP_LOGE(TAG, "spi slave bus free failed\n");
    }
    return ret;
}

static void esp_spi_deinit(interface_handle_t *handle)
{
    esp_err_t ret = ESP_OK;

    if (s_spi_post_process_task_hdl) {
        vTaskDelete(s_spi_post_process_task_hdl);
        s_spi_post_process_task_hdl = NULL;
    }

    ret = spi_slave_free(ESP_SPI_CONTROLLER);
    if (ESP_OK != ret) {
        ESP_LOGE(TAG, "spi slave bus free failed\n");
        return;
    }

    ret = spi_bus_free(ESP_SPI_CONTROLLER);
    if (ESP_OK != ret) {
        ESP_LOGE(TAG, "spi all bus free failed\n");
        return;
    }

    for (int i = 0; i < SPI_SLOT_NUM; i++) {
        if (s_slots[i].dma_tx) {
            free(s_slots[i].dma_tx);
            s_slots[i].dma_tx = NULL;
        }
        s_slots[i].in_use = false;
        s_slots[i].has_tx = false;
    }

    if (s_emergency_rx_buf) {
        free(s_emergency_rx_buf);
        s_emergency_rx_buf = NULL;
        s_emergency_rx_in_use = false;
    }

    if (s_rx_buf_pool) {
        uint8_t *rx_buf = NULL;
        while (xQueueReceive(s_rx_buf_pool, &rx_buf, 0) == pdTRUE) {
            if (rx_buf) {
                free(rx_buf);
            }
        }
        vQueueDelete(s_rx_buf_pool);
        s_rx_buf_pool = NULL;
    }
}
