// SPDX-License-Identifier: Apache-2.0
/*
 * Stack-agnostic RCP byte-stream bridge for ESP-Hosted.
 *
 * An ESP-IDF custom-RCP adapter can use read()/notify() for Host->RCP and
 * send_to_host() for RCP->Host without knowing anything about SDIO or SPI.
 */
#include "sdkconfig.h"

#ifdef CONFIG_ESP_HOSTED_RCP

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "interface.h"
#include "adapter.h"
#include "esp_rcp.h"


#define ESP_HOSTED_RCP_TX_POOL_SIZE 8

typedef struct {
    uint8_t storage[ESP_HOSTED_RCP_TX_HEADROOM + ESP_HOSTED_RCP_CHUNK_MAX];
} esp_hosted_rcp_tx_slot_t;

typedef struct {
    size_t len;
    size_t off;
    uint8_t data[];
} esp_hosted_rcp_chunk_t;

static const char *TAG = "FW_RCP";
static QueueHandle_t s_rx_queue;
static esp_hosted_rcp_chunk_t *s_current_rx;
static QueueHandle_t s_tx_free_queue;
static QueueHandle_t s_preboot_tx_queue;
static SemaphoreHandle_t s_tx_gate_lock;
static esp_hosted_rcp_tx_slot_t *s_tx_pool;
static volatile bool s_bridge_enabled;
static volatile bool s_host_ready;
static volatile bool s_host_session_ready;
static uint64_t s_session_nonce;
static esp_hosted_rcp_rx_notify_cb_t s_rx_notify;
static void *s_rx_notify_arg;

static void esp_hosted_rcp_release_tx_slot(void *handle)
{
    esp_hosted_rcp_tx_slot_t *slot = handle;

    if (!slot || !s_tx_free_queue)
        return;

    if (xQueueSend(s_tx_free_queue, &slot, 0) != pdTRUE)
        ESP_LOGE(TAG, "RCP TX pool accounting error");
}

esp_err_t esp_hosted_rcp_init(void)
{
    int i;

    if (!s_rx_queue) {
        s_rx_queue = xQueueCreate(CONFIG_ESP_HOSTED_RCP_RX_QUEUE_SIZE,
                                  sizeof(esp_hosted_rcp_chunk_t *));
        if (!s_rx_queue)
            return ESP_ERR_NO_MEM;
    }

    if (!s_tx_free_queue) {
        s_tx_free_queue = xQueueCreate(ESP_HOSTED_RCP_TX_POOL_SIZE,
                                       sizeof(esp_hosted_rcp_tx_slot_t *));
        if (!s_tx_free_queue)
            return ESP_ERR_NO_MEM;
    }

    if (!s_preboot_tx_queue) {
        s_preboot_tx_queue = xQueueCreate(ESP_HOSTED_RCP_TX_POOL_SIZE,
                                          sizeof(interface_buffer_handle_t));
        if (!s_preboot_tx_queue)
            return ESP_ERR_NO_MEM;
    }

    if (!s_tx_gate_lock) {
        s_tx_gate_lock = xSemaphoreCreateMutex();
        if (!s_tx_gate_lock)
            return ESP_ERR_NO_MEM;
    }

    if (!s_tx_pool) {
        s_tx_pool = heap_caps_calloc(
            ESP_HOSTED_RCP_TX_POOL_SIZE, sizeof(*s_tx_pool),
            MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (!s_tx_pool) {
            ESP_LOGE(TAG,
                     "Failed to reserve RCP TX pool bytes=%u free_internal=%u largest_internal=%u",
                     (unsigned)(ESP_HOSTED_RCP_TX_POOL_SIZE * sizeof(*s_tx_pool)),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            return ESP_ERR_NO_MEM;
        }

        for (i = 0; i < ESP_HOSTED_RCP_TX_POOL_SIZE; i++) {
            esp_hosted_rcp_tx_slot_t *slot = &s_tx_pool[i];

            if (xQueueSend(s_tx_free_queue, &slot, 0) != pdTRUE) {
                ESP_LOGE(TAG, "Failed to initialize RCP TX free pool");
                return ESP_FAIL;
            }
        }
    }

    do {
        s_session_nonce = ((uint64_t)esp_random() << 32) | esp_random();
    } while (!s_session_nonce);

    s_host_ready = false;
    s_host_session_ready = false;
    s_bridge_enabled = true;
    return ESP_OK;
}

void esp_hosted_rcp_deinit(void)
{
    esp_hosted_rcp_chunk_t *chunk = NULL;

    s_bridge_enabled = false;
    s_host_ready = false;
    s_host_session_ready = false;
    s_session_nonce = 0;
    if (s_preboot_tx_queue) {
        interface_buffer_handle_t pending = {0};

        while (xQueueReceive(s_preboot_tx_queue, &pending, 0) == pdTRUE) {
            if (pending.free_buf_handle && pending.priv_buffer_handle)
                pending.free_buf_handle(pending.priv_buffer_handle);
        }
    }
    if (s_current_rx) {
        free(s_current_rx);
        s_current_rx = NULL;
    }
    if (s_rx_queue) {
        while (xQueueReceive(s_rx_queue, &chunk, 0) == pdTRUE)
            free(chunk);
    }
    /*
     * TX pool resources intentionally live for the firmware lifetime. A slot
     * can still be owned by the transport when a backend is torn down, and
     * deleting the free queue would make its later release callback unsafe.
     */
    s_rx_notify = NULL;
    s_rx_notify_arg = NULL;
}

esp_err_t esp_hosted_rcp_rx_from_host(const uint8_t *data, size_t len)
{
    esp_hosted_rcp_chunk_t *chunk;

    if (!data || !len)
        return ESP_ERR_INVALID_ARG;

    /*
     * Until the exact current nonce is accepted there is no valid H2E RCP
     * stream. Stale DATA from an older host generation is therefore discarded.
     * Linux does not complete open() until the nonce-matched confirmation is
     * received, so legitimate current-generation DATA cannot reach this path.
     */
    if (!s_bridge_enabled || !s_rx_queue || !s_host_session_ready) {
        ESP_LOGW(TAG, "Dropping pre-session H2E RCP bytes len=%u",
                 (unsigned)len);
        return ESP_OK;
    }

#ifdef CONFIG_ESP_HOSTED_RCP_LOOPBACK_TEST
#if CONFIG_ESP_HOSTED_RCP_LOOPBACK_DELAY_MS > 0
    vTaskDelay(pdMS_TO_TICKS(CONFIG_ESP_HOSTED_RCP_LOOPBACK_DELAY_MS));
#endif
    return esp_hosted_rcp_send_to_host(data, len);
#endif

    chunk = malloc(sizeof(*chunk) + len);
    if (!chunk)
        return ESP_ERR_NO_MEM;

    chunk->len = len;
    chunk->off = 0;
    memcpy(chunk->data, data, len);

    /* All-or-error: never publish a partial Spinel stream chunk. */
    if (xQueueSend(s_rx_queue, &chunk, 0) != pdTRUE) {
        free(chunk);
        return ESP_ERR_NO_MEM;
    }

    if (s_rx_notify)
        s_rx_notify(s_rx_notify_arg);
    return ESP_OK;
}

uint64_t esp_hosted_rcp_session_nonce(void)
{
    return s_session_nonce;
}

bool esp_hosted_rcp_fail_closed_required(void)
{
    /*
     * Generic transport corruption is RCP-fatal only after the current host
     * has established this boot's exact nonce. Before that point there is no
     * valid H2E RCP byte stream to preserve.
     */
    return s_bridge_enabled && s_host_session_ready;
}

static esp_err_t esp_hosted_rcp_send_packet_to_host(const uint8_t *data,
                                                      size_t len,
                                                      uint8_t packet_type);

esp_err_t esp_hosted_rcp_session_start_from_host(uint8_t version,
                                                 uint64_t nonce)
{
    if (version != ESP_RCP_SESSION_VERSION || !nonce ||
        nonce != s_session_nonce) {
        ESP_LOGW(TAG,
                 "Ignoring stale RCP session marker version=%u nonce=%08x%08x expected=%08x%08x",
                 version, (unsigned)(nonce >> 32), (unsigned)nonce,
                 (unsigned)(s_session_nonce >> 32),
                 (unsigned)s_session_nonce);
        return ESP_OK;
    }

    if (!s_bridge_enabled || !s_tx_gate_lock)
        return ESP_OK;

    if (xSemaphoreTake(
            s_tx_gate_lock,
            pdMS_TO_TICKS(ESP_HOSTED_RCP_TX_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    /*
     * The exact nonce establishes the H2E stream. Confirm the same nonce back
     * to Linux before its open() is allowed to succeed.
     */
    s_host_session_ready = true;

    xSemaphoreGive(s_tx_gate_lock);

    /*
     * Runtime radio selection starts the RCP before /dev/esp_rcp0 is opened.
     * The exact nonce marker is therefore the first point where the host has
     * proved it owns this incarnation. Release any buffered startup Spinel
     * only now, before acknowledging the session.
     */
    {
        esp_err_t ret = esp_hosted_rcp_host_ready();

        if (ret != ESP_OK)
            return ret;
    }

    {
        struct esp_rcp_session_marker ack = {0};
        uint64_t nonce_le = htole64(nonce);
        esp_err_t ret;

        ack.version = ESP_RCP_SESSION_VERSION;
        memcpy(&ack.nonce, &nonce_le, sizeof(nonce_le));
        ret = esp_hosted_rcp_send_packet_to_host(
            (const uint8_t *)&ack, sizeof(ack), PACKET_TYPE_RCP_SESSION);
        if (ret != ESP_OK)
            return ret;
    }

    ESP_LOGI(TAG, "Host RCP generation confirmed nonce=%08x%08x",
             (unsigned)(nonce >> 32), (unsigned)nonce);
    return ESP_OK;
}

size_t esp_hosted_rcp_read(uint8_t *buf, size_t buf_size)
{
    size_t copied = 0;

    if (!buf || !buf_size || !s_rx_queue)
        return 0;

    while (copied < buf_size) {
        size_t available;
        size_t n;

        if (!s_current_rx &&
            xQueueReceive(s_rx_queue, &s_current_rx, 0) != pdTRUE)
            break;

        available = s_current_rx->len - s_current_rx->off;
        n = (buf_size - copied < available) ?
            (buf_size - copied) : available;
        memcpy(buf + copied, s_current_rx->data + s_current_rx->off, n);
        s_current_rx->off += n;
        copied += n;

        if (s_current_rx->off == s_current_rx->len) {
            free(s_current_rx);
            s_current_rx = NULL;
        }
    }
    return copied;
}

void esp_hosted_rcp_set_rx_notify(esp_hosted_rcp_rx_notify_cb_t cb, void *arg)
{
    s_rx_notify = cb;
    s_rx_notify_arg = arg;
}

static esp_err_t esp_hosted_rcp_send_packet_to_host(const uint8_t *data,
                                                      size_t len,
                                                      uint8_t packet_type)
{
    size_t done = 0;

    if (!data && len)
        return ESP_ERR_INVALID_ARG;
    if (!s_bridge_enabled || !s_tx_free_queue || !s_tx_pool)
        return ESP_ERR_INVALID_STATE;

    while (done < len) {
        interface_buffer_handle_t buf_handle = {0};
        esp_hosted_rcp_tx_slot_t *slot = NULL;
        size_t chunk_len = len - done;
        uint8_t *payload;

        if (chunk_len > ESP_HOSTED_RCP_CHUNK_MAX)
            chunk_len = ESP_HOSTED_RCP_CHUNK_MAX;

        if (xQueueReceive(
                s_tx_free_queue, &slot,
                pdMS_TO_TICKS(ESP_HOSTED_RCP_TX_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "RCP TX pool stalled after %u/%u bytes",
                     (unsigned)done, (unsigned)len);
            esp_hosted_rcp_fatal_restart("RCP TX pool exhausted");
            return ESP_ERR_TIMEOUT;
        }

        payload = slot->storage + ESP_HOSTED_RCP_TX_HEADROOM;
        memcpy(payload, data + done, chunk_len);

        buf_handle.if_type = ESP_RCP_IF;
        buf_handle.if_num = 0;
        buf_handle.pkt_type = packet_type;
        buf_handle.payload = payload;
        buf_handle.payload_len = chunk_len;
        buf_handle.priv_buffer_handle = slot;
        buf_handle.free_buf_handle = esp_hosted_rcp_release_tx_slot;

        if (xSemaphoreTake(
                s_tx_gate_lock,
                pdMS_TO_TICKS(ESP_HOSTED_RCP_TX_TIMEOUT_MS)) != pdTRUE) {
            esp_hosted_rcp_release_tx_slot(slot);
            ESP_LOGE(TAG, "RCP TX gate stalled after %u/%u bytes",
                     (unsigned)done, (unsigned)len);
            esp_hosted_rcp_fatal_restart("RCP TX gate timeout");
            return ESP_ERR_TIMEOUT;
        }

        if (s_host_ready) {
            if (send_to_host_timeout(
                    PRIO_Q_MID, &buf_handle,
                    pdMS_TO_TICKS(ESP_HOSTED_RCP_TX_TIMEOUT_MS)) != pdTRUE) {
                xSemaphoreGive(s_tx_gate_lock);
                esp_hosted_rcp_release_tx_slot(slot);
                ESP_LOGE(TAG, "RCP TX queue stalled after %u/%u bytes",
                         (unsigned)done, (unsigned)len);
                esp_hosted_rcp_fatal_restart("RCP Hosted TX queue timeout");
                return ESP_ERR_TIMEOUT;
            }
        } else {
            if (xQueueSend(
                    s_preboot_tx_queue, &buf_handle,
                    pdMS_TO_TICKS(ESP_HOSTED_RCP_TX_TIMEOUT_MS)) != pdTRUE) {
                xSemaphoreGive(s_tx_gate_lock);
                esp_hosted_rcp_release_tx_slot(slot);
                ESP_LOGE(TAG, "RCP preboot TX queue stalled after %u/%u bytes",
                         (unsigned)done, (unsigned)len);
                esp_hosted_rcp_fatal_restart("RCP preboot TX queue timeout");
                return ESP_ERR_TIMEOUT;
            }
        }
        xSemaphoreGive(s_tx_gate_lock);
        done += chunk_len;
    }
    return ESP_OK;
}

esp_err_t esp_hosted_rcp_send_to_host(const uint8_t *data, size_t len)
{
    return esp_hosted_rcp_send_packet_to_host(data, len, PACKET_TYPE_DATA);
}

esp_err_t esp_hosted_rcp_host_ready(void)
{
    interface_buffer_handle_t pending = {0};

    if (!s_bridge_enabled || !s_preboot_tx_queue || !s_tx_gate_lock)
        return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(
            s_tx_gate_lock,
            pdMS_TO_TICKS(ESP_HOSTED_RCP_TX_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    while (xQueueReceive(s_preboot_tx_queue, &pending, 0) == pdTRUE) {
        if (send_to_host_timeout(
                PRIO_Q_MID, &pending,
                pdMS_TO_TICKS(ESP_HOSTED_RCP_TX_TIMEOUT_MS)) != pdTRUE) {
            if (pending.free_buf_handle && pending.priv_buffer_handle)
                pending.free_buf_handle(pending.priv_buffer_handle);
            xSemaphoreGive(s_tx_gate_lock);
            ESP_LOGE(TAG, "Failed to publish buffered RCP startup bytes");
            esp_hosted_rcp_fatal_restart("buffered startup Spinel publication failed");
            return ESP_ERR_TIMEOUT;
        }
    }

    /*
     * The gate lock prevents a producer from observing READY until every
     * preboot item has been published, so the initial RESET cannot be
     * overtaken by later Spinel traffic.
     */
    s_host_ready = true;
    xSemaphoreGive(s_tx_gate_lock);
    return ESP_OK;
}

#endif /* CONFIG_ESP_HOSTED_RCP */
