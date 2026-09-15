// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2021 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include <rom/rtc.h>
#include "esp_log.h"
#include "interface.h"
#include "esp.h"
#include "sdio_slave_api.h"
#include "driver/sdio_slave.h"
#include "soc/sdio_slave_periph.h"
#include "endian.h"
#include "freertos/semphr.h"
#include "stats.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "soc/sdio_slc_host_reg.h"
#include "esp_fw_version.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#include "hal/sdio_slave_ll.h"

#define SDIO_DMA_ALIGNMENT_BYTES    4
#define SDIO_DMA_ALIGNMENT_MASK     (SDIO_DMA_ALIGNMENT_BYTES - 1)
#define IS_SDIO_DMA_ALIGNED(val)    (!((uint32_t)(val) & SDIO_DMA_ALIGNMENT_MASK))

uint32_t rx_buf_size = 15872;
uint32_t sdio_tx_aggr_size = 15872;
static uint8_t *sdio_slave_rx_buffer[RX_BUF_NUM];

static interface_context_t context;
static interface_handle_t if_handle_g;
static const char TAG[] = "FW_SDIO_SLAVE";

static interface_handle_t * sdio_init(void);
static int32_t sdio_write(interface_handle_t *handle, interface_buffer_handle_t *buf_handle);
static int sdio_read(interface_handle_t *if_handle, interface_buffer_handle_t *buf_handle);
static esp_err_t sdio_reset(interface_handle_t *handle);
static void sdio_deinit(interface_handle_t *handle);

static uint8_t gpio_oob = CONFIG_HOST_WAKEUP_GPIO;
extern volatile uint8_t power_save_on;
extern SemaphoreHandle_t wakeup_sem;

/*
 * Host-event bits arrive in sdio_intr_host() ISR context. The ISR only
 * records them and notifies this task; OPEN/CLOSE/PS/RESET all run here.
 *
 * TX admission is separate from physical completion: sdio_slave_transmit()
 * waits in send_get_finished() until the host consumes the transfer, so a
 * mutex held across that wait deadlocks the E2H-fail -> ESP_RESET path.
 * Reset blocks new submissions, lets IDF send_flush_data() unwind the
 * in-flight transmitter, then drains leftover tokens.
 */
static volatile uint32_t s_sdio_host_events;
static portMUX_TYPE s_sdio_evt_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_sdio_ctrl_stop;
static volatile TaskHandle_t s_sdio_ctrl_task;
static portMUX_TYPE s_sdio_tx_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_sdio_tx_blocked;
static volatile int s_sdio_tx_inflight;
static volatile bool s_sdio_tx_wait_idle;
static SemaphoreHandle_t s_sdio_tx_idle;

static if_ops_t if_ops = {
    .init = sdio_init,
    .write = sdio_write,
    .read = sdio_read,
    .reset = sdio_reset,
    .deinit = sdio_deinit,
};

interface_context_t *interface_insert_driver(int (*event_handler)(uint8_t val))
{
    ESP_LOGI(TAG, "Using SDIO interface");
    memset(&context, 0, sizeof(context));

    context.type = SDIO;
    context.if_ops = &if_ops;
    context.event_handler = event_handler;

    return &context;
}

int interface_remove_driver()
{
    memset(&context, 0, sizeof(context));
    return 0;
}

static bool sdio_tx_try_admit(void)
{
    bool ok;

    portENTER_CRITICAL(&s_sdio_tx_mux);
    ok = !s_sdio_tx_blocked && !s_sdio_ctrl_stop;
    if (ok)
        s_sdio_tx_inflight++;
    portEXIT_CRITICAL(&s_sdio_tx_mux);
    return ok;
}

static void sdio_tx_release(void)
{
    bool wake = false;

    portENTER_CRITICAL(&s_sdio_tx_mux);
    if (s_sdio_tx_inflight > 0)
        s_sdio_tx_inflight--;
    if (s_sdio_tx_inflight == 0 && s_sdio_tx_wait_idle)
        wake = true;
    portEXIT_CRITICAL(&s_sdio_tx_mux);
    if (wake && s_sdio_tx_idle)
        xSemaphoreGive(s_sdio_tx_idle);
}

static esp_err_t sdio_transmit_admitted(uint8_t *addr, size_t len)
{
    esp_err_t ret;

    if (!sdio_tx_try_admit())
        return ESP_ERR_INVALID_STATE;
    ret = sdio_slave_transmit(addr, len);
    sdio_tx_release();
    return ret;
}

static esp_err_t sdio_reset_hw(void)
{
    uint8_t gen;
    void *arg;
    esp_err_t ret;
    int inflight;
    int pass;

    gen = sdio_slave_read_reg(0);

    portENTER_CRITICAL(&s_sdio_tx_mux);
    s_sdio_tx_blocked = true;
    portEXIT_CRITICAL(&s_sdio_tx_mux);

    sdio_slave_stop();
    ret = sdio_slave_reset();
    if (ret != ESP_OK)
        goto fail;

    /* Flush unblocks a transmitter already inside sdio_slave_transmit().
     * A transmitter that admitted but has not queued yet can race this
     * first flush; retry rather than wait behind host completion. */
    for (pass = 0; pass < 4; pass++) {
        portENTER_CRITICAL(&s_sdio_tx_mux);
        inflight = s_sdio_tx_inflight;
        s_sdio_tx_wait_idle = (inflight > 0);
        portEXIT_CRITICAL(&s_sdio_tx_mux);

        if (!inflight)
            break;

        if (s_sdio_tx_idle &&
            xSemaphoreTake(s_sdio_tx_idle, pdMS_TO_TICKS(250)) == pdTRUE)
            continue;

        sdio_slave_stop();
        ret = sdio_slave_reset();
        if (ret != ESP_OK)
            goto fail;
    }

    portENTER_CRITICAL(&s_sdio_tx_mux);
    inflight = s_sdio_tx_inflight;
    s_sdio_tx_wait_idle = false;
    portEXIT_CRITICAL(&s_sdio_tx_mux);
    if (inflight) {
        ret = ESP_ERR_TIMEOUT;
        goto fail;
    }

    /* No transmitter is waiting; leftover flush tokens indicate dropped E2H state. */
    bool dropped_e2h_tokens = false;
    while (sdio_slave_send_get_finished(&arg, 0) == ESP_OK) {
        dropped_e2h_tokens = true;
        (void)arg;
    }

    ret = sdio_slave_start();
    if (ret != ESP_OK)
        goto fail;

    ret = sdio_slave_write_reg(1, gen);
    if (ret != ESP_OK)
        goto fail;

    portENTER_CRITICAL(&s_sdio_tx_mux);
    s_sdio_tx_blocked = false;
    portEXIT_CRITICAL(&s_sdio_tx_mux);

    if (dropped_e2h_tokens) {
        ESP_LOGE(TAG, "SDIO reset dropped committed E2H tokens; restarting firmware to ensure state agreement");
        esp_restart();
    }

    return ESP_OK;

fail:
    portENTER_CRITICAL(&s_sdio_tx_mux);
    s_sdio_tx_wait_idle = false;
    portEXIT_CRITICAL(&s_sdio_tx_mux);
    ESP_LOGE(TAG, "SDIO reset failed ret=0x%x gen=%u", ret, gen);
    return ret;
}

static void sdio_process_host_bits(uint32_t bits)
{
    if (bits & (1u << ESP_CLOSE_DATA_PATH)) {
        if (context.event_handler)
            context.event_handler(ESP_CLOSE_DATA_PATH);
    }

    if (bits & (1u << ESP_RESET)) {
        (void)sdio_reset_hw();
        ESP_LOGI(TAG, "ESP_RESET received; restarting firmware to guarantee clean application state");
        esp_restart();
    }

    if (bits & (1u << ESP_POWER_SAVE_ON)) {
        if (context.event_handler)
            context.event_handler(ESP_POWER_SAVE_ON);
        (void)sdio_reset_hw();
    }

    if (bits & (1u << ESP_POWER_SAVE_OFF)) {
        if (context.event_handler)
            context.event_handler(ESP_POWER_SAVE_OFF);
        (void)sdio_reset_hw();
    }

    if (bits & (1u << ESP_OPEN_DATA_PATH)) {
        if (context.event_handler)
            context.event_handler(ESP_OPEN_DATA_PATH);
    }
}

static void sdio_ctrl_worker(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_sdio_ctrl_stop)
            break;

        for (;;) {
            uint32_t bits;

            portENTER_CRITICAL(&s_sdio_evt_mux);
            bits = s_sdio_host_events;
            s_sdio_host_events = 0;
            portEXIT_CRITICAL(&s_sdio_evt_mux);
            if (!bits)
                break;
            sdio_process_host_bits(bits);
            if (s_sdio_ctrl_stop)
                break;
        }
        if (s_sdio_ctrl_stop)
            break;
    }

    s_sdio_ctrl_task = NULL;
    vTaskDelete(NULL);
}

/* Invoked from sdio_intr_host() ISR. Must not block or call event_handler. */
IRAM_ATTR static void event_cb(uint8_t val)
{
    BaseType_t hp_task = pdFALSE;
    TaskHandle_t task;

    if (val >= 32)
        return;

    portENTER_CRITICAL_ISR(&s_sdio_evt_mux);
    s_sdio_host_events |= (1u << val);
    portEXIT_CRITICAL_ISR(&s_sdio_evt_mux);

    task = s_sdio_ctrl_task;
    if (task)
        vTaskNotifyGiveFromISR(task, &hp_task);
    portYIELD_FROM_ISR(hp_task);
}

static void sdio_free_rx_buffers(void)
{
    for (int i = 0; i < RX_BUF_NUM; i++) {
        if (sdio_slave_rx_buffer[i]) {
            free(sdio_slave_rx_buffer[i]);
            sdio_slave_rx_buffer[i] = NULL;
        }
    }
}

static void sdio_ctrl_stop_worker(void)
{
    int i;

    s_sdio_ctrl_stop = true;
    portENTER_CRITICAL(&s_sdio_tx_mux);
    s_sdio_tx_blocked = true;
    portEXIT_CRITICAL(&s_sdio_tx_mux);

    if (wakeup_sem)
        xSemaphoreGive(wakeup_sem);

    if (s_sdio_ctrl_task) {
        TaskHandle_t task = s_sdio_ctrl_task;

        xTaskNotifyGive(task);
        for (i = 0; i < 200 && s_sdio_ctrl_task; i++)
            vTaskDelay(pdMS_TO_TICKS(10));
        if (s_sdio_ctrl_task) {
            vTaskDelete(s_sdio_ctrl_task);
            s_sdio_ctrl_task = NULL;
        }
    }
}

static void sdio_ctrl_wait_tx_idle(void)
{
    int i;

    for (i = 0; i < 50 && s_sdio_tx_inflight; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
}

static void sdio_ctrl_delete_sync(void)
{
    if (s_sdio_tx_idle) {
        vSemaphoreDelete(s_sdio_tx_idle);
        s_sdio_tx_idle = NULL;
    }

    if (wakeup_sem) {
        vSemaphoreDelete(wakeup_sem);
        wakeup_sem = NULL;
    }
}

static void sdio_ctrl_teardown(void)
{
    sdio_ctrl_stop_worker();
    sdio_ctrl_delete_sync();
}

static void sdio_read_done(void *handle)
{
    esp_err_t ret;

    if (!handle) {
        ESP_LOGE(TAG, "SDIO_H2E_RELOAD_ERROR: null RX buffer handle");
        return;
    }

    ret = sdio_slave_recv_load_buf((sdio_slave_buf_handle_t) handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDIO_H2E_RELOAD_ERROR: handle=%p ret=0x%x",
                 handle, ret);
    }
}

static interface_handle_t * sdio_init(void)
{
    esp_err_t ret = ESP_OK;
    sdio_slave_buf_handle_t handle = {0};

    /* Dynamically calculate RX_BUF_SIZE and SDIO_TX_AGGR_SIZE based on hardware bitfields */

    /*
     * rx_buf_size — max SDIO CMD53 payload per transfer
     *
     * Sized from two hardware limits:
     *
     * 1) ESP SDIO slave DMA descriptor: size and length are 14/12-bit fields
     *    (sdio_slave_ll_desc_t), so one descriptor can cover at most 2^14 - 1 = 16383/4095 bytes.
     *
     * 2) SDIO block mode (ESP_BLOCK_SIZE = 512): CMD53 block transfers must be
     *    multiples of 512 bytes.
     *
     * Largest 512-byte-aligned size that fits in one descriptor:
     * esp32c6/esp32c5/esp32c61
     *   floor(16383 / 512) = 31 blocks
     *   31 * 512 = 15872
     * (32 * 512 = 16384 exceeds the 14-bit limit.)
     * esp32:
     *   floor(4095 / 512) = 7 blocks
     *   7 * 512 = 3584
     *
     */

    sdio_slave_ll_desc_t dummy;
    memset(&dummy, 0xFF, sizeof(dummy));
    rx_buf_size = (dummy.size / 512) * 512;
    sdio_tx_aggr_size = rx_buf_size;
    ESP_LOGI(TAG, "Calculated RX_BUF_SIZE dynamically: %lu (desc.size limit: %lu)", (unsigned long)rx_buf_size, (unsigned long)dummy.size);

    sdio_slave_config_t config = {
        .sending_mode       = SDIO_SLAVE_SEND_STREAM,
        .send_queue_size    = SDIO_SLAVE_QUEUE_SIZE,
        .recv_buffer_size   = RX_BUF_SIZE,
        .event_cb           = event_cb,
        /* Note: For small devkits there may be no pullups on the board.
           This enables the internal pullups to help evaluate the driver
           quickly. However the internal pullups are not sufficient and not
           reliable, please make sure external pullups are connected to the
           bus in your real design.
           */
        //.flags              = SDIO_SLAVE_FLAG_INTERNAL_PULLUP,
        /* Note: Sometimes the SDIO card is detected but gets problem in
         * Read/Write or handling ISR because of SDIO timing issues.
         * In these cases, Please tune timing below using value from
         * https://github.com/espressif/esp-idf/blob/release/v5.0/components/hal/include/hal/sdio_slave_types.h#L26-L38
         * */
#if defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32C61)
        .timing             = SDIO_SLAVE_TIMING_NSEND_PSAMPLE,
#endif
    };
#ifdef CONFIG_SDIO_DEFAULT_SPEED
    config.flags |= SDIO_SLAVE_FLAG_DEFAULT_SPEED;
#endif

    /* Configuration for the OOB line */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1 << gpio_oob)
    };

    s_sdio_ctrl_stop = false;
    s_sdio_tx_blocked = false;
    s_sdio_tx_inflight = 0;
    s_sdio_tx_wait_idle = false;
    s_sdio_host_events = 0;

    wakeup_sem = xSemaphoreCreateBinary();
    if (wakeup_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore\n");
        return NULL;
    }

    xSemaphoreGive(wakeup_sem);

    s_sdio_tx_idle = xSemaphoreCreateBinary();
    if (!s_sdio_tx_idle) {
        ESP_LOGE(TAG, "Failed to create SDIO TX idle semaphore");
        sdio_ctrl_teardown();
        return NULL;
    }
    {
        TaskHandle_t ctrl_task = NULL;

        if (xTaskCreate(sdio_ctrl_worker, "sdio_ctrl", TASK_DEFAULT_STACK_SIZE, NULL,
                        TASK_DEFAULT_PRIO + 1, &ctrl_task) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to create SDIO control task");
            sdio_ctrl_teardown();
            return NULL;
        }
        s_sdio_ctrl_task = ctrl_task;
    }

    ret = sdio_slave_initialize(&config);
    if (ret != ESP_OK) {
        sdio_ctrl_teardown();
        return NULL;
    }

    gpio_config(&io_conf);

    for (int i = 0; i < RX_BUF_NUM; i++) {
        sdio_slave_rx_buffer[i] = heap_caps_malloc(rx_buf_size, MALLOC_CAP_DMA);
        assert(sdio_slave_rx_buffer[i] != NULL);
        handle = sdio_slave_recv_register_buf(sdio_slave_rx_buffer[i]);
        assert(handle != NULL);

        ret = sdio_slave_recv_load_buf(handle);
        if (ret != ESP_OK) {
            sdio_ctrl_stop_worker();
            sdio_slave_deinit();
            sdio_free_rx_buffers();
            sdio_ctrl_delete_sync();
            return NULL;
        }
    }

    sdio_slave_set_host_intena(SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET |
                               SDIO_SLAVE_HOSTINT_BIT0 |
                               SDIO_SLAVE_HOSTINT_BIT1 |
                               SDIO_SLAVE_HOSTINT_BIT2 |
                               SDIO_SLAVE_HOSTINT_BIT3 |
                               SDIO_SLAVE_HOSTINT_BIT4 |
                               SDIO_SLAVE_HOSTINT_BIT5 |
                               SDIO_SLAVE_HOSTINT_BIT6 |
                               SDIO_SLAVE_HOSTINT_BIT7);

    ret = sdio_slave_start();
    if (ret != ESP_OK) {
        sdio_ctrl_stop_worker();
        sdio_slave_deinit();
        sdio_free_rx_buffers();
        sdio_ctrl_delete_sync();
        return NULL;
    }

    memset(&if_handle_g, 0, sizeof(if_handle_g));
    if_handle_g.state = INIT;

    return &if_handle_g;
}

void oobTimerCallback(TimerHandle_t xTimer)
{
    xTimerDelete(xTimer, 0);
    WRITE_PERI_REG(GPIO_OUT_W1TC_REG, (1 << gpio_oob));
}

void wake_host()
{
    TimerHandle_t xTimer = NULL;
    esp_err_t ret = ESP_OK;
    uint8_t retry = 1;

    ESP_LOGI(TAG, "WAKE UP Host!!!!!\n");

    while (retry) {
        WRITE_PERI_REG(GPIO_OUT_W1TS_REG, (1 << gpio_oob));
        xTimer = xTimerCreate("Timer", pdMS_TO_TICKS(10), pdFALSE, 0, oobTimerCallback);
        if (xTimer == NULL) {
            ESP_LOGE(TAG, "Failed to create timer for SDIO OOB");
        }
        ret = xTimerStart(xTimer, 0);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to start timer for SDIO OOB");
        }

        if (wakeup_sem) {
            /* wait for host resume */
            ret = xSemaphoreTake(wakeup_sem, pdMS_TO_TICKS(100));

            if (ret == pdPASS) {
                /*                             usleep(100*1000);*/
                xSemaphoreGive(wakeup_sem);
                break;
            }
        }

        retry--;
    }
}

static int32_t sdio_write(interface_handle_t *handle, interface_buffer_handle_t *buf_handle)
{
    esp_err_t ret = ESP_OK;
    int32_t total_len = 0;
    uint8_t* sendbuf = NULL;
    uint16_t offset = 0;
    struct esp_payload_header *header = NULL;
    bool free_sendbuf = false;

    if (!handle || !buf_handle) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_FAIL;
    }

    if (handle->state != ACTIVE) {
        return ESP_FAIL;
    }

    if (power_save_on) {
        return ESP_FAIL;
    }

    if (!buf_handle->payload_len || !buf_handle->payload) {
        ESP_LOGE(TAG, "Invalid arguments, len:%d", buf_handle->payload_len);
        return ESP_FAIL;
    }


    uint32_t align_padding = 0;
    offset = sizeof(struct esp_payload_header);
    if (IS_WIFI_DATA_PACKET(buf_handle)) {
        /* As Wi-Fi esf-buf has headroom of rx_ctrl before Wi-Fi data pointer, we can use that space to store packet header.
         * This way we do not need to alloc and memcpy again */
        uint32_t payload_addr = (uint32_t)buf_handle->payload;
        align_padding = (SDIO_DMA_ALIGNMENT_BYTES - (payload_addr % SDIO_DMA_ALIGNMENT_BYTES)) % SDIO_DMA_ALIGNMENT_BYTES;
        sendbuf = (uint8_t *)buf_handle->payload - sizeof(struct esp_payload_header) - align_padding;
        if (!esp_ptr_dma_capable(sendbuf) || !IS_SDIO_DMA_ALIGNED(sendbuf)) {
            align_padding = 0;
            sendbuf = heap_caps_malloc(buf_handle->payload_len + offset, MALLOC_CAP_DMA);
            if (sendbuf == NULL) {
                ESP_LOGE(TAG, "Malloc send buffer fail!");
                return ESP_ERR_NO_MEM;
            }
            memcpy(sendbuf + offset, buf_handle->payload, buf_handle->payload_len);
            free_sendbuf = true;
        }
    } else {

        sendbuf = heap_caps_malloc(buf_handle->payload_len + offset, MALLOC_CAP_DMA);
        if (sendbuf == NULL) {
            ESP_LOGE(TAG, "Malloc send buffer fail!");
            return ESP_ERR_NO_MEM;
        }

        memcpy(sendbuf + offset, buf_handle->payload, buf_handle->payload_len);

        if (buf_handle->free_buf_handle && buf_handle->payload) {
            buf_handle->free_buf_handle(buf_handle->payload);
        }

        buf_handle->priv_buffer_handle = sendbuf;
        buf_handle->free_buf_handle = heap_caps_free;
    }

    total_len = buf_handle->payload_len + offset + align_padding;
    header = (struct esp_payload_header *)sendbuf;
    memset(header, 0, sizeof(struct esp_payload_header) + align_padding);

    /* Initialize header */
    header->if_type = buf_handle->if_type;
    header->if_num = buf_handle->if_num;
    header->len = htole16(buf_handle->payload_len);
    header->reserved2 = buf_handle->flag;
    header->offset = htole16(sizeof(struct esp_payload_header) + align_padding);
    header->packet_type = buf_handle->pkt_type;
    if (header->if_type == ESP_TEST_IF) {
        debug_raw_tp_set_seq(header, buf_handle->raw_tp_seq);
    }

#if CONFIG_ESP_SDIO_CHECKSUM
    header->checksum = htole16(compute_checksum(sendbuf,
                                                total_len));
#endif

    ret = sdio_transmit_admitted(sendbuf, total_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdio slave transmit error, ret : 0x%x\r\n", ret);
        if (free_sendbuf) {
            heap_caps_free(sendbuf);
        }
        return ret;
    }
    if (free_sendbuf) {
        heap_caps_free(sendbuf);
    }
#if 0
    ESP_LOGE(TAG, "\nTo Host");
    ESP_LOG_BUFFER_HEXDUMP("s->h", buf_handle->payload,
                           buf_handle->payload_len, ESP_LOG_INFO);
#endif

    return buf_handle->payload_len;
}

int32_t sdio_write_aggr(interface_handle_t *handle, uint8_t *payload,
                        uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;

    if (!handle || !payload || !payload_len) {
        ESP_LOGE(TAG, "Invalid aggregate write arguments");
        return ESP_FAIL;
    }

    if (handle->state != ACTIVE || power_save_on) {
        return ESP_FAIL;
    }

    ret = sdio_transmit_admitted(payload, payload_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdio slave aggregate transmit error, ret: 0x%x\r\n", ret);
        return ret;
    }

    return payload_len;
}

esp_err_t send_bootup_event_to_host(uint8_t cap)
{
    struct esp_payload_header *header = NULL;
    struct esp_internal_bootup_event *event = NULL;
    struct fw_data * fw_p = NULL;
    interface_buffer_handle_t buf_handle = {0};
    uint8_t * pos = NULL;
    esp_err_t ret = ESP_OK;
    uint16_t len = 0;

    memset(&buf_handle, 0, sizeof(buf_handle));

    buf_handle.payload = heap_caps_malloc(RX_BUF_SIZE, MALLOC_CAP_DMA);
    assert(buf_handle.payload);
    memset(buf_handle.payload, 0, RX_BUF_SIZE);

    header = (struct esp_payload_header *) buf_handle.payload;

    header->if_type = ESP_INTERNAL_IF;
    header->if_num = 0;
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

    /* TLV - Capability */
    *pos = ESP_BOOTUP_CAPABILITY;         pos++; len++;
    *pos = LENGTH_1_BYTE;                 pos++; len++;
    *pos = cap;                           pos++; len++;

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
    ESP_LOGI(TAG, "last reset cause: %0xx", rtc_get_reset_reason(0));
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

    /* payload len = Event len + sizeof(event len) */
    len += 1;
    event->header.len = htole16(len);

    header->len = htole16(buf_handle.payload_len - sizeof(struct esp_payload_header));

#if CONFIG_ESP_SDIO_CHECKSUM
    header->checksum = htole16(compute_checksum(buf_handle.payload, buf_handle.payload_len));
#endif

    ret = sdio_transmit_admitted(buf_handle.payload, buf_handle.payload_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdio slave tx error, ret : 0x%x\r\n", ret);
        free(buf_handle.payload);
        return ret;
    }

    free(buf_handle.payload);
    return ESP_OK;
}

static int sdio_read(interface_handle_t *if_handle, interface_buffer_handle_t *buf_handle)
{
    esp_err_t ret;
    size_t sdio_read_len = 0;

    if (!if_handle || !buf_handle) {
        ESP_LOGE(TAG, "Invalid arguments to sdio_read");
        return ESP_FAIL;
    }

    if (if_handle->state != ACTIVE) {
        return ESP_FAIL;
    }

    buf_handle->sdio_buf_handle = NULL;
    buf_handle->payload = NULL;
    buf_handle->payload_len = 0;
    ret = sdio_slave_recv(&(buf_handle->sdio_buf_handle),
                          &(buf_handle->payload), &(sdio_read_len),
                          portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SDIO_H2E_RECV_ERROR: ret=0x%x len=%u handle=%p payload=%p",
                 ret, (unsigned int)sdio_read_len,
                 buf_handle->sdio_buf_handle, buf_handle->payload);
        return ESP_FAIL;
    }
    if (!buf_handle->sdio_buf_handle || !buf_handle->payload ||
        !sdio_read_len) {
        ESP_LOGE(TAG, "SDIO_H2E_RECV_ERROR: invalid completion len=%u "
                 "handle=%p payload=%p",
                 (unsigned int)sdio_read_len,
                 buf_handle->sdio_buf_handle, buf_handle->payload);
        if (buf_handle->sdio_buf_handle) {
            sdio_read_done(buf_handle->sdio_buf_handle);
            buf_handle->sdio_buf_handle = NULL;
            buf_handle->payload = NULL;
        }
        return ESP_FAIL;
    }
    buf_handle->payload_len = sdio_read_len & 0xFFFF;

    buf_handle->free_buf_handle = sdio_read_done;
#if 0
    ESP_LOGE(TAG, "\nFrom Host");
    ESP_LOG_BUFFER_HEXDUMP("h->s", buf_handle->payload, buf_handle->payload_len, ESP_LOG_INFO);
#endif
    return buf_handle->payload_len;
}

static esp_err_t sdio_reset(interface_handle_t *handle)
{
    (void)handle;
    return sdio_reset_hw();
}

static void sdio_deinit(interface_handle_t *handle)
{
    (void)handle;

    sdio_ctrl_stop_worker();
    sdio_slave_stop();
    sdio_slave_reset();
    sdio_ctrl_wait_tx_idle();
    sdio_free_rx_buffers();
    sdio_ctrl_delete_sync();
}
