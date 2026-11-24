// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2023 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sys/queue.h"
#include "soc/soc.h"
#include "soc/soc_caps.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <unistd.h>
#ifndef CONFIG_IDF_TARGET_ARCH_RISCV
#include "xtensa/core-macros.h"
#endif
#include "esp_private/wifi.h"
#include "esp.h"
#include "interface.h"
#if CONFIG_ESP_SDIO_HOST_INTERFACE
#include "sdio_slave_api.h"
#endif
#include "esp_wpa.h"
#include "app_main.h"
#include "esp_wifi.h"
#include "cmd.h"
#include "esp_mac.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#ifdef CONFIG_BT_ENABLED
#include "esp_bt.h"
#ifdef CONFIG_BT_HCI_UART_NO
#include "driver/uart.h"
#endif
#endif
#include "endian.h"
#ifdef CONFIG_ESP_HOSTED_RCP
#include "esp_rcp.h"
#endif
#ifdef CONFIG_ESP_THREAD_RCP_UART
#include "thread_rcp_uart.h"
#endif

#include "slave_bt.c"
#include "stats.h"
#include "esp_mac.h"

static const char TAG[] = "FW_MAIN";
#define WIFI_TX_RETRY_COUNT       20
#define WIFI_TX_RETRY_DELAY_US    1000

#if CONFIG_ESP_WLAN_DEBUG
static const char TAG_RX[] = "H -> S";
static const char TAG_TX[] = "S -> H";
#endif

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#define STATS_TICKS         pdMS_TO_TICKS(1000*2)
#define ARRAY_SIZE_OFFSET   5
#endif

volatile uint8_t datapath = 0;
volatile uint8_t association_ongoing = 0;
volatile uint8_t station_connected = 0;
volatile uint8_t station_authorized = 0;
volatile uint8_t softap_started = 0;
volatile uint8_t ota_ongoing = 0;
volatile uint8_t power_save_on = 0;
struct wow_config wow;
SemaphoreHandle_t wakeup_sem;
SemaphoreHandle_t init_sem;
uint32_t ip_address;
struct macfilter_list mac_list;

#ifdef ESP_DEBUG_STATS
uint32_t from_wlan_count = 0;
uint32_t to_host_count = 0;
uint32_t to_host_sent_count = 0;
#endif

interface_context_t *if_context = NULL;
interface_handle_t *if_handle = NULL;

QueueHandle_t to_host_queue[MAX_PRIORITY_QUEUES] = {NULL};

#ifdef CONFIG_ESP_HOSTED_RCP
static portMUX_TYPE s_rcp_fatal_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_rcp_fatal_pending;
static TaskHandle_t s_rcp_fatal_task;
static const char *s_rcp_fatal_reason;

static void esp_hosted_rcp_fatal_restart_task(void *arg)
{
    (void)arg;

    for (;;) {
        const char *reason;

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        portENTER_CRITICAL(&s_rcp_fatal_mux);
        reason = s_rcp_fatal_reason;
        portEXIT_CRITICAL(&s_rcp_fatal_mux);

        ESP_LOGE(TAG, "RCP fatal recovery: %s",
                 reason ? reason : "unspecified failure");

        /*
         * Yield one tick so the task that detected the failure can return its
         * current RX/TX buffer before the transport is quiesced.
         */
        vTaskDelay(1);

        datapath = 0;
        if (if_handle)
            if_handle->state = DEACTIVE;

#if CONFIG_ESP_SDIO_HOST_INTERFACE
        {
            esp_err_t ret = sdio_prepare_fatal_reboot();

            if (ret != ESP_OK)
                ESP_LOGE(TAG, "RCP pre-reboot SDIO quiesce failed: %s",
                         esp_err_to_name(ret));
        }
#endif

        esp_restart();
    }
}

static esp_err_t esp_hosted_rcp_fatal_worker_init(void)
{
    if (s_rcp_fatal_task)
        return ESP_OK;

    if (xTaskCreate(esp_hosted_rcp_fatal_restart_task, "rcp_fatal",
                    TASK_DEFAULT_STACK_SIZE, NULL,
                    TASK_DEFAULT_PRIO + 2, &s_rcp_fatal_task) != pdTRUE) {
        s_rcp_fatal_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void esp_hosted_rcp_fatal_restart(const char *reason)
{
    TaskHandle_t task = NULL;
    bool schedule = false;

    portENTER_CRITICAL(&s_rcp_fatal_mux);
    if (s_rcp_fatal_task && !s_rcp_fatal_pending) {
        s_rcp_fatal_pending = true;
        s_rcp_fatal_reason = reason;
        task = s_rcp_fatal_task;
        schedule = true;
    }
    portEXIT_CRITICAL(&s_rcp_fatal_mux);

    if (!schedule) {
        if (!s_rcp_fatal_task)
            ESP_LOGE(TAG, "RCP fatal worker unavailable; RCP must remain disabled");
        return;
    }

    /*
     * Close the datapath synchronously. The pre-created recovery worker then
     * performs ownership cleanup without allocating memory in the failure path.
     */
    datapath = 0;
    if (if_handle)
        if_handle->state = DEACTIVE;

    xTaskNotifyGive(task);
}
#endif


static uint8_t s_radio_service = ESP_RADIO_SERVICE_NONE;
static bool s_radio_service_command_seen;

void esp_radio_service_snapshot(uint8_t *active, uint64_t *rcp_nonce)
{
    if (active)
        *active = s_radio_service;
    if (rcp_nonce) {
#ifdef CONFIG_ESP_HOSTED_RCP
        *rcp_nonce = (s_radio_service & ESP_RADIO_SERVICE_IEEE802154) ?
            esp_hosted_rcp_session_nonce() : 0;
#else
        *rcp_nonce = 0;
#endif
    }
}

esp_err_t esp_radio_service_apply(uint8_t requested, uint8_t *active,
                                  uint8_t *flags, uint64_t *rcp_nonce)
{
    esp_err_t ret = ESP_OK;
    uint8_t add_mask;
#ifdef CONFIG_BT_ENABLED
    bool bt_started_now = false;
#endif

    if (requested & ~ESP_RADIO_SERVICE_MASK)
        return ESP_ERR_INVALID_ARG;

    s_radio_service_command_seen = true;

    if (flags)
        *flags = 0;

    if (requested == s_radio_service) {
        esp_radio_service_snapshot(active, rcp_nonce);
        return ESP_OK;
    }

    /*
     * Adding a radio service is safe in-place: the IDF coexistence arbiter
     * handles Wi-Fi, Bluetooth and IEEE 802.15.4 sharing the S31 RF.
     *
     * Removing a running service still crosses controller/OpenThread lifetime
     * boundaries. Keep that transition fail-closed via the existing controlled
     * firmware reincarnation until every backend has a proven hot-stop path.
     */
    if (s_radio_service & ~requested) {
        if (flags)
            *flags |= ESP_RADIO_SERVICE_F_RESTART_REQUIRED;
        esp_radio_service_snapshot(active, rcp_nonce);
        return ESP_OK;
    }

    add_mask = requested & ~s_radio_service;

    if (add_mask & ESP_RADIO_SERVICE_BT) {
#if defined(CONFIG_BT_ENABLED) && defined(BLUETOOTH_HCI)
        ret = initialise_bluetooth();
        if (ret != ESP_OK)
            goto out;
        s_radio_service |= ESP_RADIO_SERVICE_BT;
        bt_started_now = true;
#else
        /*
         * HCI-over-UART is a physically independent interface. It is started
         * at boot and is deliberately outside ESP-Hosted radio-service
         * control.
         */
        ret = ESP_ERR_NOT_SUPPORTED;
        goto out;
#endif
    }

    if (add_mask & ESP_RADIO_SERVICE_IEEE802154) {
#ifdef CONFIG_ESP_HOSTED_RCP
        ret = esp_hosted_rcp_fatal_worker_init();
        if (ret != ESP_OK)
            goto rollback_bt;

        ret = esp_hosted_rcp_init();
        if (ret != ESP_OK)
            goto rollback_bt;

#if defined(CONFIG_ESP_HOSTED_RCP_OPENTHREAD)
        ret = esp_hosted_rcp_openthread_start();
        if (ret != ESP_OK) {
            esp_hosted_rcp_deinit();
            goto rollback_bt;
        }
#elif !defined(CONFIG_ESP_HOSTED_RCP_LOOPBACK_TEST)
        esp_hosted_rcp_deinit();
        ret = ESP_ERR_NOT_SUPPORTED;
        goto rollback_bt;
#endif
        s_radio_service |= ESP_RADIO_SERVICE_IEEE802154;
#else
        ret = ESP_ERR_NOT_SUPPORTED;
        goto rollback_bt;
#endif
    }

    goto out;

rollback_bt:
#ifdef CONFIG_BT_ENABLED
    if (bt_started_now) {
        deinitialize_bluetooth();
        s_radio_service &= ~ESP_RADIO_SERVICE_BT;
    }
#endif

out:
    esp_radio_service_snapshot(active, rcp_nonce);
    return ret;
}

/* send_task handle so producers can wake it via xTaskNotifyGive instead of
 * the send_task polling every tick (vTaskDelay(1)). NULL until the task starts. */
TaskHandle_t send_task_handle = NULL;

#ifdef ESP_DEBUG_STATS
static uint32_t h2e_rx_pkts;
static uint32_t h2e_rx_bytes;
static uint32_t h2e_wifi_tx_ok;
static uint32_t h2e_wifi_tx_fail;
static uint32_t h2e_wifi_tx_retries;
static uint32_t h2e_drop_invalid;
static uint32_t h2e_drop_checksum;
static uint32_t h2e_drop_not_ready;
static TickType_t h2e_last_stats_tick;

#define H2E_STATS_INC(counter) do { counter++; } while (0)
#define H2E_STATS_ADD(counter, value) do { counter += (value); } while (0)

static void print_h2e_stats(void)
{
    TickType_t now = xTaskGetTickCount();

    if (now - h2e_last_stats_tick < pdMS_TO_TICKS(5000)) {
        return;
    }

    h2e_last_stats_tick = now;
    if (h2e_rx_pkts || h2e_wifi_tx_ok || h2e_wifi_tx_fail ||
        h2e_drop_invalid || h2e_drop_checksum || h2e_drop_not_ready) {
        ESP_LOGD(TAG, "H2E stats: rx=%lu bytes=%lu wifi_ok=%lu wifi_fail=%lu wifi_retries=%lu invalid=%lu csum=%lu not_ready=%lu",
                 (unsigned long)h2e_rx_pkts, (unsigned long)h2e_rx_bytes,
                 (unsigned long)h2e_wifi_tx_ok, (unsigned long)h2e_wifi_tx_fail,
                 (unsigned long)h2e_wifi_tx_retries,
                 (unsigned long)h2e_drop_invalid, (unsigned long)h2e_drop_checksum,
                 (unsigned long)h2e_drop_not_ready);
        h2e_rx_pkts = 0;
        h2e_rx_bytes = 0;
        h2e_wifi_tx_ok = 0;
        h2e_wifi_tx_fail = 0;
        h2e_wifi_tx_retries = 0;
        h2e_drop_invalid = 0;
        h2e_drop_checksum = 0;
        h2e_drop_not_ready = 0;
    }
}
#else
#define H2E_STATS_INC(counter) do { } while (0)
#define H2E_STATS_ADD(counter, value) do { } while (0)
static inline void print_h2e_stats(void) { }
#endif

static int wifi_tx_with_retry(wifi_interface_t wifi_if, uint8_t *payload,
                              uint16_t payload_len)
{
    int ret = 0;
    uint16_t retry = 0;

    do {
        ret = esp_wifi_internal_tx(wifi_if, payload, payload_len);
        if (ret != ESP_ERR_NO_MEM) {
            break;
        }
        H2E_STATS_INC(h2e_wifi_tx_retries);
        usleep(WIFI_TX_RETRY_DELAY_US);
    } while (++retry < WIFI_TX_RETRY_COUNT);

    return ret;
}

#if CONFIG_ESP_SPI_HOST_INTERFACE || CONFIG_ESP_SPI_HD_HOST_INTERFACE
#ifdef CONFIG_IDF_TARGET_ESP32S2
#define TO_HOST_QUEUE_SIZE      5
#else
#define TO_HOST_QUEUE_SIZE      20
#endif
#else
#define TO_HOST_QUEUE_SIZE      100
#endif

#define ETH_DATA_LEN            1500
#define MONITOR_DROP_LOG_INTERVAL 1024U

static volatile bool s_monitor_enabled;
static uint8_t s_monitor_channel; /* last successfully configured channel */
static bool s_monitor_channel_explicit; /* zero-channel request inherits STA/AP */
static uint8_t s_monitor_rx_channel; /* last observed captured frame */
static uint32_t s_monitor_rx_packets;
static uint32_t s_monitor_rx_bytes;
/* Callback vs successfully enqueued counts distinguish Wi-Fi firmware filtering
 * from later firmware queue/transport losses without per-frame logging. */
static uint32_t s_monitor_cb_mgmt;
static uint32_t s_monitor_cb_ctrl;
static uint32_t s_monitor_cb_data;
static uint32_t s_monitor_tx_mgmt;
static uint32_t s_monitor_tx_ctrl;
static uint32_t s_monitor_tx_data;
static uint32_t s_monitor_queue_drops;
static uint32_t s_monitor_alloc_drops;
static uint32_t s_monitor_oversize_drops;

static void monitor_log_drop(const char *reason)
{
    uint32_t drops = s_monitor_queue_drops + s_monitor_alloc_drops +
                     s_monitor_oversize_drops;

    if ((drops % MONITOR_DROP_LOG_INTERVAL) == 1U) {
        ESP_LOGW(TAG, "MONITOR_DROP reason=%s queue=%"PRIu32" alloc=%"PRIu32
                 " oversize=%"PRIu32" rx=%"PRIu32" bytes=%"PRIu32,
                 reason, s_monitor_queue_drops, s_monitor_alloc_drops,
                 s_monitor_oversize_drops, s_monitor_rx_packets,
                 s_monitor_rx_bytes);
    }
}

static uint8_t monitor_normalize_phy(const wifi_pkt_rx_ctrl_t *rx)
{
#if CONFIG_SOC_WIFI_HE_SUPPORT
    switch (rx->cur_bb_format) {
    case RX_BB_FORMAT_HT:
        return 1;
    case RX_BB_FORMAT_VHT:
    case RX_BB_FORMAT_VHT_MU:
        return 2;
    case RX_BB_FORMAT_HE_SU:
    case RX_BB_FORMAT_HE_MU:
    case RX_BB_FORMAT_HE_ERSU:
    case RX_BB_FORMAT_HE_TB:
        return 3;
    default:
        return 0;
    }
#else
    if (rx->sig_mode == 1)
        return 1;
    if (rx->sig_mode == 3)
        return 2;
    return 0;
#endif
}

static void monitor_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = buf;
    const wifi_pkt_rx_ctrl_t *rx;
    interface_buffer_handle_t out = {0};
    struct esp_monitor_meta_v1 *meta;
    uint8_t *payload;
    uint16_t frame_len;
    size_t total_len;

    if (!s_monitor_enabled || !buf)
        return;

    /* Keep callback counts ahead of resource checks: if the Wi-Fi library
     * delivers control frames but the transport does not, we can prove it. */
    switch (type) {
    case WIFI_PKT_MGMT:
        s_monitor_cb_mgmt++;
        break;
    case WIFI_PKT_CTRL:
        s_monitor_cb_ctrl++;
        if (s_monitor_cb_ctrl == 1U)
            ESP_LOGI(TAG, "MONITOR_CTRL_FIRST_RX received from Wi-Fi library");
        break;
    case WIFI_PKT_DATA:
        s_monitor_cb_data++;
        break;
    default:
        return;
    }
    if (!datapath || !to_host_queue[PRIO_Q_LOW])
        return;

    rx = &pkt->rx_ctrl;
    if (!rx->sig_len)
        return;

    frame_len = (uint16_t)rx->sig_len;
    total_len = sizeof(*meta) + frame_len;
    if (total_len > MAX_ALLOWED_BUF_PAYLOAD_LEN || total_len > UINT16_MAX) {
        s_monitor_oversize_drops++;
        monitor_log_drop("oversize");
        return;
    }

    /* Keep sniffer load bounded by the existing low-priority transport queue.
     * Never block the Wi-Fi task waiting for host transport capacity. */
    if (!uxQueueSpacesAvailable(to_host_queue[PRIO_Q_LOW])) {
        s_monitor_queue_drops++;
        monitor_log_drop("queue-full");
        return;
    }

    payload = heap_caps_malloc(total_len, MALLOC_CAP_DMA);
    if (!payload) {
        s_monitor_alloc_drops++;
        monitor_log_drop("alloc");
        return;
    }

    meta = (struct esp_monitor_meta_v1 *)payload;
    memset(meta, 0, sizeof(*meta));
    meta->version = ESP_MONITOR_META_VERSION;
    meta->frame_type = (uint8_t)type;
    meta->channel = (uint8_t)rx->channel;
    if (meta->channel)
        s_monitor_rx_channel = meta->channel;
    meta->rssi = (int8_t)rx->rssi;
    meta->rate = 0xff;
    meta->sig_mode = monitor_normalize_phy(rx);
    meta->mcs = 0xff;
    meta->antenna = 0xff;
#if CONFIG_SOC_WIFI_HE_SUPPORT
    /*
     * HE-capable targets use esp_wifi_rxctrl_t, whose layout is selected by
     * the target's Wi-Fi MAC version.  Access only fields common to those
     * IDF-defined layouts instead of interpreting raw descriptor bytes.
     *
     * In this descriptor family "rate" is the 11b transmission-rate field
     * only for RX_BB_FORMAT_11B and is the L-SIG Rate field otherwise; it is
     * not the legacy wifi_phy_rate_t encoding consumed by our v1 metadata.
     * Leave rate unknown rather than publishing a fabricated radiotap rate.
     */
    meta->cwb = rx->second != WIFI_SECOND_CHAN_NONE;
    meta->secondary_channel = (uint8_t)rx->second;
    if (rx->cur_bb_format == RX_BB_FORMAT_11B)
        meta->flags |= ESP_MONITOR_FLAG_CCK;
    if (rx->dump_len && rx->sig_len == rx->dump_len + 4)
        meta->flags |= ESP_MONITOR_FLAG_FCS_PRESENT;
#else
    /*
     * Legacy targets expose the older wifi_pkt_rx_ctrl_t descriptor where
     * rate/MCS/bandwidth/GI/antenna are directly documented by ESP-IDF.
     */
    meta->rate = (uint8_t)rx->rate;
    meta->mcs = (uint8_t)rx->mcs;
    meta->cwb = (uint8_t)rx->cwb;
    meta->secondary_channel = (uint8_t)rx->secondary_channel;
    meta->antenna = (uint8_t)rx->ant;
    if (rx->sgi)
        meta->flags |= ESP_MONITOR_FLAG_SHORT_GI;
    if (rx->sig_mode == 0 &&
        (rx->rate == WIFI_PHY_RATE_1M_L ||
         rx->rate == WIFI_PHY_RATE_2M_L ||
         rx->rate == WIFI_PHY_RATE_2M_S ||
         rx->rate == WIFI_PHY_RATE_5M_L ||
         rx->rate == WIFI_PHY_RATE_5M_S ||
         rx->rate == WIFI_PHY_RATE_11M_L ||
         rx->rate == WIFI_PHY_RATE_11M_S))
        meta->flags |= ESP_MONITOR_FLAG_CCK;
    meta->flags |= ESP_MONITOR_FLAG_FCS_PRESENT;
#endif
    if (meta->sig_mode == 1)
        meta->flags |= ESP_MONITOR_FLAG_HT;
    meta->frame_len = htole16(frame_len);
    meta->timestamp_us = htole32((uint32_t)rx->timestamp);
    memcpy(payload + sizeof(*meta), pkt->payload, frame_len);

    out.if_type = ESP_MONITOR_IF;
    out.if_num = 0;
    out.payload = payload;
    out.payload_len = (uint16_t)total_len;
    out.pkt_type = PACKET_TYPE_MONITOR;
    out.priv_buffer_handle = payload;
    out.free_buf_handle = free;

    if (send_to_host_timeout(PRIO_Q_LOW, &out, 0) != pdTRUE) {
        s_monitor_queue_drops++;
        monitor_log_drop("queue-race");
        free(payload);
        return;
    }

    s_monitor_rx_packets++;
    s_monitor_rx_bytes += frame_len;
    switch (type) {
    case WIFI_PKT_MGMT:
        s_monitor_tx_mgmt++;
        break;
    case WIFI_PKT_CTRL:
        s_monitor_tx_ctrl++;
        break;
    case WIFI_PKT_DATA:
        s_monitor_tx_data++;
        break;
    default:
        break;
    }

    /* A bounded UART trace allows hardware tests to diagnose whether
     * control frames reach the Wi-Fi callback without shutting down mon0. */
    if ((s_monitor_rx_packets & 2047U) == 0U) {
        ESP_LOGI(TAG, "MONITOR_TYPE_COUNTS cb_mgmt=%"PRIu32
                 " cb_ctrl=%"PRIu32" cb_data=%"PRIu32
                 " queued_mgmt=%"PRIu32" queued_ctrl=%"PRIu32
                 " queued_data=%"PRIu32" queue_drop=%"PRIu32,
                 s_monitor_cb_mgmt, s_monitor_cb_ctrl, s_monitor_cb_data,
                 s_monitor_tx_mgmt, s_monitor_tx_ctrl, s_monitor_tx_data,
                 s_monitor_queue_drops);
    }
}

bool esp_hosted_monitor_is_enabled(void)
{
    return s_monitor_enabled;
}

/*
 * Preserve the *operating* channel while a shared STA/AP still owns it.
 * Deinit disconnects STA before stopping Wi-Fi; after that point IDF may
 * already report the monitor-only fallback channel instead of the channel
 * on which the connected STA was operating. Keep the inherited/pinned
 * distinction unchanged by this snapshot.
 */
void esp_hosted_monitor_snapshot_channel(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    wifi_ap_record_t associated_ap = {0};
    uint8_t current_channel = 0;
    bool connected_sta;
    bool active_ap;

    if (!s_monitor_enabled || s_monitor_channel_explicit)
        return;
    if (esp_wifi_get_mode(&mode) != ESP_OK)
        return;

    connected_sta = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) &&
                    esp_wifi_sta_get_ap_info(&associated_ap) == ESP_OK;
    /* AP interfaces are configured in APSTA mode by Hosted. Require an
     * active AP_START event before treating APSTA-without-STA as an owner;
     * APSTA mode alone is not proof that the radio is operating an AP. */
    active_ap = mode == WIFI_MODE_AP ||
                (mode == WIFI_MODE_APSTA && softap_started);
    if (!connected_sta && !active_ap)
        return;
    if (esp_wifi_get_channel(&current_channel, &second) == ESP_OK &&
        current_channel) {
        if (current_channel != s_monitor_channel)
            ESP_LOGI(TAG, "MONITOR_INHERITED_CHANNEL_SNAPSHOT %u -> %u",
                     s_monitor_channel, current_channel);
        s_monitor_channel = current_channel;
    }
}

uint8_t esp_hosted_monitor_channel(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    wifi_ap_record_t connected_ap = {0};
    uint8_t current_channel = 0;

    if (!s_monitor_enabled || s_monitor_channel_explicit)
        return s_monitor_channel;

    /* Capture with an inherited channel follows the actual radio. RX
     * metadata cannot update configuration: queued old packets may arrive
     * after a legitimate STA/AP channel transition. */
    if (esp_wifi_get_mode(&mode) == ESP_OK &&
        (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA ||
         (mode == WIFI_MODE_STA &&
          esp_wifi_sta_get_ap_info(&connected_ap) == ESP_OK)) &&
        esp_wifi_get_channel(&current_channel, &second) == ESP_OK &&
        current_channel)
        return current_channel;

    return s_monitor_channel;
}

bool esp_hosted_monitor_channel_allowed(uint8_t requested, bool radio_transition)
{
    if (!s_monitor_enabled)
        return true;
    if (!requested)
        return false;
    /* Only explicitly pinned monitor channels constrain STA/AP roaming or
     * setup. For inherited monitoring, the owner of the shared radio may
     * retune it; passive scans must remain on the live operating channel. */
    if (!s_monitor_channel_explicit && radio_transition)
        return true;
    return requested == esp_hosted_monitor_channel();
}

esp_err_t esp_hosted_monitor_restore(void)
{
    if (!s_monitor_enabled)
        return ESP_OK;
    if (!s_monitor_channel)
        return ESP_ERR_INVALID_STATE;
    return esp_hosted_monitor_config(true,
                                     s_monitor_channel_explicit ?
                                     s_monitor_channel : 0);
}

esp_err_t esp_hosted_monitor_config(bool enable, uint8_t channel)
{
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_CTRL |
                       WIFI_PROMIS_FILTER_MASK_DATA,
    };
    wifi_promiscuous_filter_t ctrl_filter = {
        .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL,
    };
    wifi_promiscuous_filter_t readback = {0};
    wifi_promiscuous_filter_t old_filter = {0};
    wifi_promiscuous_filter_t old_ctrl_filter = {0};
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    wifi_second_chan_t second_channel = WIFI_SECOND_CHAN_NONE;
    wifi_ap_record_t associated_ap = {0};
    bool sta_owns_channel = false;
    bool radio_owns_channel;
    uint8_t operating_channel = 0;
    uint8_t previous_channel = 0;
    wifi_second_chan_t previous_second = WIFI_SECOND_CHAN_NONE;
    uint8_t effective_channel = channel;
    bool was_enabled = s_monitor_enabled;
    bool rollback_failed = false;
    esp_err_t ret;

    if (!enable) {
        bool was_enabled = s_monitor_enabled;

        s_monitor_enabled = false;
        ret = esp_wifi_set_promiscuous(false);
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
            s_monitor_enabled = was_enabled;
            return ret;
        }
        ESP_LOGI(TAG, "Monitor disabled: rx=%"PRIu32" bytes=%"PRIu32
                 " queue_drop=%"PRIu32" alloc_drop=%"PRIu32
                 " oversize_drop=%"PRIu32,
                 s_monitor_rx_packets, s_monitor_rx_bytes,
                 s_monitor_queue_drops, s_monitor_alloc_drops,
                 s_monitor_oversize_drops);
        ESP_LOGI(TAG, "MONITOR_TYPE_COUNTS cb_mgmt=%"PRIu32
                 " cb_ctrl=%"PRIu32" cb_data=%"PRIu32
                 " queued_mgmt=%"PRIu32" queued_ctrl=%"PRIu32
                 " queued_data=%"PRIu32,
                 s_monitor_cb_mgmt, s_monitor_cb_ctrl, s_monitor_cb_data,
                 s_monitor_tx_mgmt, s_monitor_tx_ctrl, s_monitor_tx_data);
        return ESP_OK;
    }

    /*
     * A zero enable-channel means "inherit the current shared-radio channel".
     * Linux cfg80211 6.1 only allows .set_monitor_channel while monitor is the
     * sole running interface, so an already-active STA/AP must be inherited
     * at monitor-open time instead of requiring an impossible userspace retune.
     */
    /* Association and scans can own the radio channel before their
     * completion events set station_connected or scan state to idle. */
    if (esp_hosted_wifi_channel_transition_active()) {
        ESP_LOGW(TAG, "MONITOR_CONFIG_REJECT channel transition active");
        return ESP_ERR_INVALID_STATE;
    }
    ret = esp_wifi_get_mode(&current_mode);
    if (ret != ESP_OK)
        return ret;

    /*
     * The AP configuration's channel is not necessarily the operating
     * channel: in APSTA mode the STA channel takes precedence.
     * Similarly softap_started can lag a synchronous mode transition.
     */
    /* Do not trust station_connected across a rapid stop/restart: its
     * WIFI_EVENT_STA_DISCONNECTED notification can lag the mode change. */
    if (current_mode == WIFI_MODE_STA || current_mode == WIFI_MODE_APSTA)
        sta_owns_channel = esp_wifi_sta_get_ap_info(&associated_ap) == ESP_OK;
    radio_owns_channel = sta_owns_channel ||
                         current_mode == WIFI_MODE_AP ||
                         current_mode == WIFI_MODE_APSTA;
    if (radio_owns_channel) {
        ret = esp_wifi_get_channel(&operating_channel, &second_channel);
        if (ret != ESP_OK)
            return ret;
        /* Never retune an active shared radio on an unresolved channel. */
        if (!operating_channel)
            return ESP_ERR_INVALID_STATE;
        if (channel && channel != operating_channel) {
            ESP_LOGW(TAG, "Monitor channel %u conflicts with operating channel %u",
                     channel, operating_channel);
            return ESP_ERR_INVALID_STATE;
        }
        effective_channel = operating_channel;
    }

    /* Channel 0 means inherit, not "switch to channel 1". If an existing
     * monitor survives WLAN teardown, restore its last real channel even
     * after the STA/AP role stops owning the radio. A genuinely new
     * monitor-only instance still starts on channel 1. */
    if (!effective_channel)
        effective_channel = was_enabled && s_monitor_channel ?
                            s_monitor_channel : 1;

    if (was_enabled) {
        /* Snapshot filters before modifying a running monitor interface.
         * A rejected channel change must not change its capture policy. */
        ret = esp_wifi_get_promiscuous_filter(&old_filter);
        if (ret != ESP_OK)
            return ret;
        ret = esp_wifi_get_promiscuous_ctrl_filter(&old_ctrl_filter);
        if (ret != ESP_OK)
            return ret;
    }
    if (!radio_owns_channel) {
        ret = esp_wifi_get_channel(&previous_channel, &previous_second);
        if (ret != ESP_OK)
            return ret;
    }

    ESP_LOGI(TAG, "MONITOR_ENABLE_STAGE filter begin channel=%u", effective_channel);
    ret = esp_wifi_set_promiscuous_filter(&filter);
    ESP_LOGI(TAG, "MONITOR_ENABLE_STAGE filter done ret=%d", ret);
    if (ret != ESP_OK)
        goto config_failed;

    /* The type mask alone does not explicitly select control subtypes.
     * Request all control subtypes so ACK/RTS/CTS/BA/BAR are not excluded. */
    ESP_LOGI(TAG, "MONITOR_CTRL_FILTER_SET mask=0x%08"PRIx32,
             ctrl_filter.filter_mask);
    ret = esp_wifi_set_promiscuous_ctrl_filter(&ctrl_filter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MONITOR_CTRL_FILTER_SET_FAILED ret=%d", ret);
        goto config_failed;
    }
    ret = esp_wifi_get_promiscuous_ctrl_filter(&readback);
    if (ret != ESP_OK || readback.filter_mask != ctrl_filter.filter_mask) {
        ESP_LOGE(TAG, "MONITOR_CTRL_FILTER_VERIFY_FAILED ret=%d expected=0x%08"PRIx32
                 " got=0x%08"PRIx32, ret, ctrl_filter.filter_mask,
                 readback.filter_mask);
        ret = ret == ESP_OK ? ESP_FAIL : ret;
        goto config_failed;
    }
    ESP_LOGI(TAG, "MONITOR_CTRL_FILTER_VERIFIED mask=0x%08"PRIx32,
             readback.filter_mask);
    memset(&readback, 0, sizeof(readback));
    ret = esp_wifi_get_promiscuous_filter(&readback);
    if (ret != ESP_OK ||
        (readback.filter_mask & filter.filter_mask) != filter.filter_mask) {
        ESP_LOGE(TAG, "MONITOR_TYPE_FILTER_VERIFY_FAILED ret=%d expected=0x%08"PRIx32
                 " got=0x%08"PRIx32, ret, filter.filter_mask, readback.filter_mask);
        ret = ret == ESP_OK ? ESP_FAIL : ret;
        goto config_failed;
    }
    ESP_LOGI(TAG, "MONITOR_TYPE_FILTER_VERIFIED mask=0x%08"PRIx32,
             readback.filter_mask);
    ESP_LOGI(TAG, "MONITOR_ENABLE_STAGE rx_callback begin");
    ret = esp_wifi_set_promiscuous_rx_cb(monitor_rx_cb);
    ESP_LOGI(TAG, "MONITOR_ENABLE_STAGE rx_callback done ret=%d", ret);
    if (ret != ESP_OK)
        goto config_failed;
    ESP_LOGI(TAG, "MONITOR_ENABLE_STAGE promiscuous_on begin");
    ret = esp_wifi_set_promiscuous(true);
    ESP_LOGI(TAG, "MONITOR_ENABLE_STAGE promiscuous_on done ret=%d", ret);
    if (ret != ESP_OK)
        goto config_failed;

    /* Commit the retune LAST: filter/verification/callback failures must
     * never move the physical channel while the host retains the old one. */
    if (!radio_owns_channel &&
        (effective_channel != previous_channel ||
         previous_second != WIFI_SECOND_CHAN_NONE)) {
        ret = esp_wifi_set_channel(effective_channel, WIFI_SECOND_CHAN_NONE);
        if (ret != ESP_OK)
            goto config_failed;
    }

    s_monitor_channel = effective_channel;
    s_monitor_channel_explicit = channel != 0;
    s_monitor_enabled = true;
    ESP_LOGI(TAG, "Monitor enabled on channel %u%s", effective_channel,
             s_monitor_channel_explicit ? " (explicit)" : " (inherited)");
    return ESP_OK;

config_failed:
    /* ESP-IDF may partially apply a command before returning a failure.
     * Restore capture filters and verify the radio did not move. If the
     * previous channel cannot be recovered, a firmware restart is safer
     * than acknowledging a failed operation with ambiguous RF state. */
    if (!radio_owns_channel) {
        uint8_t observed_channel = 0;
        wifi_second_chan_t observed_second = WIFI_SECOND_CHAN_NONE;
        esp_err_t verify = esp_wifi_get_channel(&observed_channel,
                                                &observed_second);

        if (verify != ESP_OK)
            rollback_failed = true;
        else if (observed_channel != previous_channel ||
                 observed_second != previous_second) {
            if (!previous_channel ||
                esp_wifi_set_channel(previous_channel, previous_second) != ESP_OK)
                rollback_failed = true;
            else {
                uint8_t restored_channel = 0;
                wifi_second_chan_t restored_second = WIFI_SECOND_CHAN_NONE;

                if (esp_wifi_get_channel(&restored_channel, &restored_second) != ESP_OK ||
                    restored_channel != previous_channel ||
                    restored_second != previous_second)
                    rollback_failed = true;
            }
        }
    }
    if (was_enabled) {
        esp_err_t filter_ret = esp_wifi_set_promiscuous_filter(&old_filter);
        esp_err_t ctrl_ret = esp_wifi_set_promiscuous_ctrl_filter(&old_ctrl_filter);
        esp_err_t enable_ret = esp_wifi_set_promiscuous(true);

        /* A failure in the first rollback step must not suppress the
         * independent cleanup attempts that follow it. */
        if (filter_ret != ESP_OK || ctrl_ret != ESP_OK ||
            enable_ret != ESP_OK) {
            ESP_LOGE(TAG, "MONITOR_CONFIG_FILTER_ROLLBACK_FAILED filter=%d ctrl=%d enable=%d",
                     filter_ret, ctrl_ret, enable_ret);
            rollback_failed = true;
        }
    } else {
        esp_err_t disable_ret = esp_wifi_set_promiscuous(false);

        /* A stopped radio cannot retain active capture. No need for a
         * second restart merely because rollback observes that state. */
        if (disable_ret != ESP_OK &&
            disable_ret != ESP_ERR_WIFI_NOT_STARTED &&
            disable_ret != ESP_ERR_WIFI_NOT_INIT)
            rollback_failed = true;
    }
    if (rollback_failed) {
        ESP_LOGE(TAG, "MONITOR_CONFIG_ROLLBACK_FAILED ret=%d; restarting", ret);
        esp_restart();
    }
    return ret;
}


uint8_t dev_mac[MAC_ADDR_LEN] = {0};

/* The IDF-derived STA MAC is the configured base MAC on Wi-Fi SoCs. */
void esp_hosted_add_boot_mac_tlv(uint8_t **pos, uint16_t *len)
{
    uint8_t mac[MAC_ADDR_LEN];
    uint8_t *p;
    const uint8_t count = CONFIG_ESP_MAC_UNIVERSAL_MAC_ADDRESSES;

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK || (mac[0] & 1) ||
        (count != 1 && count != 2 && count != 4))
        return;
    p = *pos;
    *p++ = ESP_BOOTUP_BASE_MAC_INFO;
    *p++ = MAC_ADDR_LEN + 1;
    memcpy(p, mac, MAC_ADDR_LEN);
    p += MAC_ADDR_LEN;
    *p++ = count;
    *pos = p;
    *len += MAC_ADDR_LEN + 3;
}


#if CONFIG_ESP_SDIO_HOST_INTERFACE
extern void wake_host();
#endif

static uint32_t get_extended_capabilities(void)
{
    uint32_t cap = 0;

#if CONFIG_SOC_WIFI_SUPPORTED
    cap |= ESP_EXT_CAP_SCAN_IE;
    ESP_LOGI(TAG, "- Probe-request IEs for host scans");
#endif

#if defined(CONFIG_ESP_HOSTED_RCP) || \
    (defined(CONFIG_BT_ENABLED) && defined(BLUETOOTH_HCI))
    cap |= ESP_EXT_CAP_RADIO_SERVICE_CTRL;
    ESP_LOGI(TAG, "- Runtime-selectable ESP-Hosted secondary-radio services");
#endif
#ifdef CONFIG_ESP_HOSTED_RCP
    cap |= ESP_EXT_CAP_RCP_AVAILABLE;
    ESP_LOGI(TAG, "   - IEEE 802.15.4 RCP available for runtime activation");
#endif
    return cap;
}

static uint32_t get_capabilities()
{
    uint32_t cap = 0;

    ESP_LOGI(TAG, "Supported features are:");
#if CONFIG_ESP_SPI_HD_HOST_INTERFACE
    ESP_LOGI(TAG, "- WLAN over SPI half-duplex/QIO");
    cap |= ESP_WLAN_SPI_SUPPORT;
#elif CONFIG_ESP_SPI_HOST_INTERFACE
    ESP_LOGI(TAG, "- WLAN over SPI");
    cap |= ESP_WLAN_SPI_SUPPORT;
#elif CONFIG_ESP_USB_HOST_INTERFACE
    ESP_LOGI(TAG, "- WLAN over USB");
    cap |= ESP_WLAN_USB_SUPPORT;
#else
    ESP_LOGI(TAG, "- WLAN over SDIO");
    cap |= ESP_WLAN_SDIO_SUPPORT;
#endif

#if CONFIG_SOC_WIFI_SUPPORTED
    ESP_LOGI(TAG, "- WLAN monitor mode");
    cap |= ESP_WLAN_MONITOR_SUPPORT;
#endif

#if SOC_WIFI_GCMP_SUPPORT
    ESP_LOGI(TAG, "- GCMP");
    cap |= ESP_WLAN_GCMP_SUPPORT;
#endif

#if CONFIG_ESP_SPI_CHECKSUM || CONFIG_ESP_SPI_HD_CHECKSUM || CONFIG_ESP_SDIO_CHECKSUM
    cap |= ESP_CHECKSUM_ENABLED;
#endif

#ifdef CONFIG_BT_ENABLED
    cap |= get_bluetooth_capabilities();
#endif
    ESP_LOGI(TAG, "Capabilities: 0x%"PRIx32, cap);

    return cap;
}

#if CONFIG_ESP_SDIO_HOST_INTERFACE
uint8_t address_lookup(uint8_t *mac_addr)
{
    int i;

    /*  ESP_LOG_BUFFER_HEXDUMP("Look up", mac_addr, MAC_ADDR_LEN, ESP_LOG_INFO);*/
    if (!mac_addr) {
        return 0;
    }

    for (i = 0; i < mac_list.count; i++) {
        /*      ESP_LOG_BUFFER_HEXDUMP("Against", mac_list.mac_addr[i], MAC_ADDR_LEN, ESP_LOG_INFO);*/
        if (memcmp(mac_list.mac_addr[i], mac_addr, MAC_ADDR_LEN) == 0) {
            return 1;
        }
    }

    return 0;
}

uint8_t is_wakeup_needed(interface_buffer_handle_t *buf_handle)
{
    uint8_t *pos;
    uint8_t *buf_start;
    uint32_t *target_ip;

    buf_start = buf_handle->payload;

    if (!buf_start) {
        /* Do not wake up */
        return 0;
    }

    //TODO #define ETH_P_ARP    0x0806
    //TODO Add Macros for below magic numbers
    if (buf_handle->payload_len >= 42) {
        pos = buf_start + 12;

        if ((*pos == 8) && (*(pos + 1) == 6)) {
            /* ARP packet */
            pos = buf_start + 38;
            target_ip = (uint32_t *) pos;
            ESP_LOG_BUFFER_HEXDUMP("ARP Target IP: ", target_ip, 4, ESP_LOG_DEBUG);
            ESP_LOG_BUFFER_HEXDUMP("ARP My IP: ", &ip_address, 4, ESP_LOG_DEBUG);

            if (memcmp(target_ip, &ip_address, 4) == 0) {
                /* Wake up host for self IP ARP request */
                ESP_LOGD(TAG, "IP matched, Wake up host");
                return 1;
            } else {
                ESP_LOGD(TAG, "IP not matched, noop");
                return 0;
            }
        }
    }

    pos = buf_start;
    if (*pos & 1) {
        /* Multicast Destination Address */
        ESP_LOG_BUFFER_HEXDUMP("Frame", pos, 32, ESP_LOG_DEBUG);

        if (address_lookup(pos)) {
            /* Multicast group is subscribed. Wake up host */
            ESP_LOGD(TAG, "Multicast addr matched, wakeup host");
            return 1;
        }

        /* Host did not subscribed this group. Do not wake host */
        ESP_LOGD(TAG, "Multicast addr did not match. noop");
        return 0;
    }

    if (memcmp(dev_mac, pos, MAC_ADDR_LEN) == 0) {
        ESP_LOG_BUFFER_HEXDUMP("Frame", pos, 32, ESP_LOG_DEBUG);
        ESP_LOGD(TAG, "Unicast addr matched, wake up host");
        return 1;
    }

    ESP_LOGD(TAG, "Default : noop");
    return 0;
}
#endif

esp_err_t wlan_ap_rx_callback(void *buffer, uint16_t len, void *eb)
{
    esp_err_t ret = ESP_OK;
    interface_buffer_handle_t buf_handle = {0};

    if (!buffer || !eb || !datapath || ota_ongoing) {
        if (eb) {
            esp_wifi_internal_free_rx_buffer(eb);
        }
        return ESP_OK;
    }

    buf_handle.if_type = ESP_AP_IF;
    buf_handle.if_num = 0;
    buf_handle.payload_len = len;
    buf_handle.payload = buffer;
    buf_handle.pkt_type = PACKET_TYPE_DATA;
    buf_handle.priv_buffer_handle = eb;
    buf_handle.free_buf_handle = esp_wifi_internal_free_rx_buffer;

    /* ESP_LOGI(TAG, "Slave -> Host: AP data packet\n"); */
    /* ESP_LOG_BUFFER_HEXDUMP("RX", buffer, len, ESP_LOG_INFO); */
    ret = xQueueSend(to_host_queue[PRIO_Q_LOW], &buf_handle, portMAX_DELAY);

    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send buffer\n");
        goto DONE;
    }

    if (send_task_handle)
        xTaskNotifyGive(send_task_handle);

    return ESP_OK;

DONE:
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

esp_err_t wlan_sta_rx_callback(void *buffer, uint16_t len, void *eb)
{
    esp_err_t ret = ESP_OK;
    interface_buffer_handle_t buf_handle = {0};

    if (!buffer || !eb || !datapath || ota_ongoing) {
        if (eb) {
            esp_wifi_internal_free_rx_buffer(eb);
        }
        return ESP_OK;
    }

#ifdef ESP_DEBUG_STATS
    from_wlan_count++;
#endif

    buf_handle.if_type = ESP_STA_IF;
    buf_handle.if_num = 0;
    buf_handle.payload_len = len;
    buf_handle.payload = buffer;
    buf_handle.pkt_type = PACKET_TYPE_DATA;
    buf_handle.priv_buffer_handle = eb;
    buf_handle.free_buf_handle = esp_wifi_internal_free_rx_buffer;

    ret = xQueueSend(to_host_queue[PRIO_Q_LOW], &buf_handle, portMAX_DELAY);

    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send buffer\n");
        goto DONE;
    }

    if (send_task_handle)
        xTaskNotifyGive(send_task_handle);

    return ESP_OK;

DONE:
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

void process_tx_pkt(interface_buffer_handle_t *buf_handle)
{
    int32_t ret = ESP_FAIL;
    bool raw_tp_frame = buf_handle && buf_handle->if_type == ESP_TEST_IF;
    bool cmd_response = false;
    uint8_t cmd_code = 0;
    uint16_t cmd_seq = 0;

    if (buf_handle && buf_handle->pkt_type == PACKET_TYPE_COMMAND_RESPONSE &&
        buf_handle->payload &&
        buf_handle->payload_len >= sizeof(struct command_header)) {
        const struct command_header *cmd =
            (const struct command_header *)buf_handle->payload;

        cmd_response = true;
        cmd_code = cmd->cmd_code;
        cmd_seq = le16toh(cmd->seq_num);
        ESP_LOGD(TAG, "CMD_RESP_DEQUEUE code=%u seq=%u len=%u",
                 cmd_code, cmd_seq, buf_handle->payload_len);
    }

    /* Check if data path is not yet open */
    if (!datapath) {
        ESP_LOGE(TAG, "E2H_DROP_DATAPATH_CLOSED if=%u pkt=%u len=%u cmd=%u/%u",
                 buf_handle ? buf_handle->if_type : 0,
                 buf_handle ? buf_handle->pkt_type : 0,
                 buf_handle ? buf_handle->payload_len : 0,
                 cmd_code, cmd_seq);
        /* Post processing */
        if (buf_handle->free_buf_handle && buf_handle->priv_buffer_handle) {
            buf_handle->free_buf_handle(buf_handle->priv_buffer_handle);
            buf_handle->priv_buffer_handle = NULL;
        }
        usleep(100 * 1000);
        return;
    }
    if (if_context && if_context->if_ops && if_context->if_ops->write) {
        if (raw_tp_frame) {
            if (!debug_raw_tp_is_run_active(buf_handle->raw_tp_run_id)) {
                if (buf_handle->free_buf_handle && buf_handle->priv_buffer_handle) {
                    buf_handle->free_buf_handle(buf_handle->priv_buffer_handle);
                    buf_handle->priv_buffer_handle = NULL;
                }
                return;
            }
            buf_handle->raw_tp_seq = debug_raw_tp_tx_seq_get();
        }
        ret = if_context->if_ops->write(if_handle, buf_handle);
        if (ret <= 0 && !raw_tp_frame) {
            ESP_LOGE(TAG, "E2H_TRANSPORT_WRITE_FAILED if=%u pkt=%u len=%u ret=%"PRId32,
                     buf_handle->if_type, buf_handle->pkt_type,
                     buf_handle->payload_len, ret);
#ifdef CONFIG_ESP_HOSTED_RCP
            if (buf_handle->if_type == ESP_RCP_IF) {
                /*
                 * Spinel is a stateful byte stream. Once an RCP packet has
                 * been handed to the physical transport, retrying it cannot
                 * prove whether the host consumed any/all bytes. Fail the
                 * firmware incarnation instead of silently desynchronizing.
                 */
                ESP_LOGE(TAG, "RCP transport delivery ambiguous");
                esp_hosted_rcp_fatal_restart("E2H transport delivery failed");
            }
#endif
        } else if (ret > 0 && cmd_response) {
            ESP_LOGD(TAG, "CMD_RESP_TRANSPORT_OK code=%u seq=%u bytes=%"PRId32,
                     cmd_code, cmd_seq, ret);
        }
#if !CONFIG_ESP_SPI_HOST_INTERFACE && !CONFIG_ESP_SPI_HD_HOST_INTERFACE
        if (raw_tp_frame) {
            if (ret == (int32_t)buf_handle->payload_len) {
                debug_raw_tp_tx_complete(1);
            } else {
                ESP_LOGE(TAG, "RAW_TP_TX_FAILED: seq=%"PRIu32
                         " transport_ret=%"PRId32,
                         buf_handle->raw_tp_seq, ret);
                debug_raw_tp_tx_failed(1);
            }
        }
#endif
    } else {
        ESP_LOGE(TAG, "E2H_TRANSPORT_UNAVAILABLE if=%u pkt=%u len=%u cmd=%u/%u",
                 buf_handle ? buf_handle->if_type : 0,
                 buf_handle ? buf_handle->pkt_type : 0,
                 buf_handle ? buf_handle->payload_len : 0,
                 cmd_code, cmd_seq);
        if (raw_tp_frame) {
            ESP_LOGE(TAG, "RAW_TP_TX_FAILED: transport write unavailable");
            debug_raw_tp_tx_failed(1);
        }
    }
    /* Post processing */
    if (buf_handle->free_buf_handle && buf_handle->priv_buffer_handle) {
        buf_handle->free_buf_handle(buf_handle->priv_buffer_handle);
        buf_handle->priv_buffer_handle = NULL;
    }
}


#ifdef CONFIG_ESP_SDIO_HOST_INTERFACE
/* send_task is the sole LOW-priority consumer. Keep one not-yet-aggregated
 * item in consumer-owned storage rather than dequeueing and racing producers
 * to put it back into a potentially full FreeRTOS queue. */
static interface_buffer_handle_t low_prio_deferred;
static bool low_prio_deferred_valid;

static void free_tx_buf_handle(interface_buffer_handle_t *buf_handle)
{
    if (buf_handle->free_buf_handle && buf_handle->priv_buffer_handle) {
        buf_handle->free_buf_handle(buf_handle->priv_buffer_handle);
        buf_handle->priv_buffer_handle = NULL;
    }
}

static void process_low_prio_tx_packets(uint16_t queued, uint8_t *aggr_buf)
{
    interface_buffer_handle_t buf_handle = {0};
    uint16_t aggr_len = 0;
    uint16_t raw_tp_frames = 0;
    bool flush_after_pkt = false;

    if (!queued && !low_prio_deferred_valid) {
        return;
    }

    if (!datapath) {
        if (low_prio_deferred_valid) {
            buf_handle = low_prio_deferred;
            memset(&low_prio_deferred, 0, sizeof(low_prio_deferred));
            low_prio_deferred_valid = false;
            process_tx_pkt(&buf_handle);
        }
        while (queued--) {
            if (xQueueReceive(to_host_queue[PRIO_Q_LOW], &buf_handle, portMAX_DELAY)) {
                process_tx_pkt(&buf_handle);
            }
        }
        return;
    }

    if (!aggr_buf) {
        if (low_prio_deferred_valid) {
            buf_handle = low_prio_deferred;
            memset(&low_prio_deferred, 0, sizeof(low_prio_deferred));
            low_prio_deferred_valid = false;
            process_tx_pkt(&buf_handle);
        } else if (xQueueReceive(to_host_queue[PRIO_Q_LOW], &buf_handle, portMAX_DELAY)) {
            process_tx_pkt(&buf_handle);
        }
        return;
    }

    while (low_prio_deferred_valid || queued ||
           uxQueueMessagesWaiting(to_host_queue[PRIO_Q_LOW])) {
        struct esp_payload_header *header = NULL;
        uint16_t frame_len = 0;
        uint16_t aligned_len = 0;
        uint16_t offset = sizeof(struct esp_payload_header);
        TickType_t wait = queued ? portMAX_DELAY : 0;

        if (low_prio_deferred_valid) {
            buf_handle = low_prio_deferred;
            memset(&low_prio_deferred, 0, sizeof(low_prio_deferred));
            low_prio_deferred_valid = false;
        } else {
            if (!xQueueReceive(to_host_queue[PRIO_Q_LOW], &buf_handle, wait)) {
                break;
            }
            if (queued) {
                queued--;
            }
        }

        if (!buf_handle.payload || !buf_handle.payload_len ||
            buf_handle.payload_len + offset > SDIO_TX_AGGR_SIZE) {
            free_tx_buf_handle(&buf_handle);
            continue;
        }

        if (buf_handle.if_type == ESP_TEST_IF &&
            !debug_raw_tp_is_run_active(buf_handle.raw_tp_run_id)) {
            free_tx_buf_handle(&buf_handle);
            continue;
        }

        frame_len = buf_handle.payload_len + offset;
        aligned_len = (frame_len + 3) & ~3;
        flush_after_pkt = buf_handle.payload_len <= SDIO_TX_LATENCY_BYPASS_SIZE;
        if (aggr_len && (flush_after_pkt ||
                         aggr_len + aligned_len > SDIO_TX_AGGR_SIZE)) {
            low_prio_deferred = buf_handle;
            low_prio_deferred_valid = true;
            memset(&buf_handle, 0, sizeof(buf_handle));
            break;
        }

        if (power_save_on && wow.magic_pkt) {
            if (is_wakeup_needed(&buf_handle)) {
                ESP_LOGI(TAG, "Wakeup on Magic packet");
                wake_host();
                buf_handle.flag = 0xFF;
            }
        }

        header = (struct esp_payload_header *) (aggr_buf + aggr_len);
        memset(header, 0, sizeof(*header));
        header->if_type = buf_handle.if_type;
        header->if_num = buf_handle.if_num;
        header->len = htole16(buf_handle.payload_len);
        header->reserved2 = buf_handle.flag;
        header->offset = htole16(offset);
        header->packet_type = buf_handle.pkt_type;
        if (header->if_type == ESP_TEST_IF) {
            /* Reserve a contiguous range locally. The global sequence is
             * committed only if this complete aggregate reaches SDIO TX
             * completion. */
            debug_raw_tp_set_seq(header, debug_raw_tp_tx_seq_get() +
                                         raw_tp_frames);
            raw_tp_frames++;
        }
        memcpy(aggr_buf + aggr_len + offset, buf_handle.payload,
               buf_handle.payload_len);
        if (aligned_len > frame_len) {
            memset(aggr_buf + aggr_len + frame_len, 0,
                   aligned_len - frame_len);
        }
#if CONFIG_ESP_SDIO_CHECKSUM
        header->checksum = htole16(compute_checksum(aggr_buf + aggr_len,
                                                    frame_len));
#endif
        aggr_len += aligned_len;
        free_tx_buf_handle(&buf_handle);
        if (flush_after_pkt) {
            break;
        }
    }

    if (aggr_len) {
        if (if_context && if_context->if_ops && if_context->if_ops->write) {
            int32_t ret = sdio_write_aggr(if_handle, aggr_buf, aggr_len);

            if (raw_tp_frames) {
                if (ret == (int32_t)aggr_len) {
                    debug_raw_tp_tx_complete(raw_tp_frames);
                } else {
                    ESP_LOGE(TAG, "RAW_TP_TX_FAILED: first_seq=%"PRIu32
                             " frames=%u transport_ret=%"PRId32,
                             debug_raw_tp_tx_seq_get(), raw_tp_frames, ret);
                    debug_raw_tp_tx_failed(raw_tp_frames);
                }
            }
        } else if (raw_tp_frames) {
            ESP_LOGE(TAG, "RAW_TP_TX_FAILED: first_seq=%"PRIu32
                     " frames=%u transport write unavailable",
                     debug_raw_tp_tx_seq_get(), raw_tp_frames);
            debug_raw_tp_tx_failed(raw_tp_frames);
        }
    }

}
#endif

esp_err_t send_to_host_timeout(uint8_t prio_q_idx, interface_buffer_handle_t *buf_handle, TickType_t wait_ticks)
{
    BaseType_t ret;

    if (prio_q_idx >= MAX_PRIORITY_QUEUES || !buf_handle ||
        !to_host_queue[prio_q_idx]) {
        ESP_LOGE(TAG, "E2H_QUEUE_INVALID prio=%u buf=%p", prio_q_idx, buf_handle);
        return ESP_ERR_INVALID_ARG;
    }

    ret = xQueueSend(to_host_queue[prio_q_idx], buf_handle, wait_ticks);
    if (ret == pdTRUE && send_task_handle)
        xTaskNotifyGive(send_task_handle);
    return ret;
}

esp_err_t send_to_host(uint8_t prio_q_idx, interface_buffer_handle_t *buf_handle)
{
    return send_to_host_timeout(prio_q_idx, buf_handle, portMAX_DELAY);
}

/* Send data to host */
void send_task(void* pvParameters)
{
#ifdef ESP_DEBUG_STATS
    int t1, t2, t_total = 0;
    int d_total = 0;
#endif
    interface_buffer_handle_t buf_handle = {0};
    uint16_t high_prio_pkt_waiting = 0;
    uint16_t mid_prio_pkt_waiting = 0;
    uint16_t low_prio_pkt_waiting = 0;
#if CONFIG_ESP_SDIO_HOST_INTERFACE
    uint8_t *sdio_aggr_buf = heap_caps_malloc(SDIO_TX_AGGR_SIZE, MALLOC_CAP_DMA);

    if (!sdio_aggr_buf) {
        ESP_LOGW(TAG, "SDIO aggregate buffer allocation failed, using single-packet path");
    }
#endif

    while (1) {
        high_prio_pkt_waiting = uxQueueMessagesWaiting(to_host_queue[PRIO_Q_HIGH]);
        mid_prio_pkt_waiting = uxQueueMessagesWaiting(to_host_queue[PRIO_Q_MID]);
        low_prio_pkt_waiting = uxQueueMessagesWaiting(to_host_queue[PRIO_Q_LOW]);

        if (high_prio_pkt_waiting) {
            while (high_prio_pkt_waiting) {
                if (xQueueReceive(to_host_queue[PRIO_Q_HIGH], &buf_handle, portMAX_DELAY)) {
                    process_tx_pkt(&buf_handle);
                }
                high_prio_pkt_waiting--;
            }
        } else if (mid_prio_pkt_waiting) {
            if (xQueueReceive(to_host_queue[PRIO_Q_MID], &buf_handle, portMAX_DELAY)) {
                process_tx_pkt(&buf_handle);
            }
        } else if (low_prio_pkt_waiting
#if CONFIG_ESP_SDIO_HOST_INTERFACE
                   || low_prio_deferred_valid
#endif
                  ) {
#if CONFIG_ESP_SDIO_HOST_INTERFACE
            process_low_prio_tx_packets(low_prio_pkt_waiting, sdio_aggr_buf);
#else
            if (xQueueReceive(to_host_queue[PRIO_Q_LOW], &buf_handle, portMAX_DELAY)) {
                process_tx_pkt(&buf_handle);
            }
#endif
        } else {
            /* All queues empty: block until a producer (RX callback or
             * send_to_host) notifies us, instead of polling every tick
             * (vTaskDelay(1) added up to 1 tick of latency per idle cycle). */
            xTaskNotifyWait(0, ULONG_MAX, NULL, portMAX_DELAY);
        }
    }
}

void process_priv_commamd(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_cmd_dispatch(if_type, payload, payload_len);
}

static bool esp_rcp_fail_corrupt_h2e(
    const struct esp_payload_header *header, const char *reason)
{
#ifdef CONFIG_ESP_HOSTED_RCP
    if (esp_hosted_rcp_fail_closed_required()) {
        ESP_LOGE(TAG, "RCP H2E stream may be truncated: %s if=%u type=%u",
                 reason, header ? header->if_type : 0xff,
                 header ? header->packet_type : 0xff);
        esp_hosted_rcp_fatal_restart(reason);
        return true;
    }
#else
    (void)header;
    (void)reason;
#endif
    return false;
}

void process_rx_pkt(interface_buffer_handle_t *buf_handle)
{
    static const struct esp_payload_header zero_padding_header;
    struct esp_payload_header *header = NULL;
    uint8_t *payload = NULL;
    uint16_t payload_len = 0;
    uint16_t offset = 0;
    uint32_t frame_len = 0;
    uint32_t aligned_len = 0;
    uint32_t pos = 0;
#if CONFIG_ESP_SDIO_CHECKSUM
    uint16_t rx_checksum = 0;
    uint16_t checksum = 0;
#endif

    while (pos + sizeof(struct esp_payload_header) <= buf_handle->payload_len) {
        header = (struct esp_payload_header *) (buf_handle->payload + pos);
        offset = le16toh(header->offset);
        payload_len = le16toh(header->len);

        if (payload_len == 0) {
            /* Linux pads each SDIO CMD53 write to a 512-byte block. Once the
             * last real aggregate frame is consumed, an all-zero header is
             * therefore the normal end-of-aggregate marker. */
            if (pos > 0 &&
                memcmp(header, &zero_padding_header, sizeof(*header)) == 0) {
                break;
            }
            H2E_STATS_INC(h2e_drop_invalid);
            ESP_LOGE(TAG, "H2E_AGGR_ERROR: non-padding zero payload length "
                     "pos=%u total=%u if=%u type=%u offset=%u flags=0x%02x "
                     "checksum=0x%04x",
                     pos, buf_handle->payload_len, header->if_type,
                     header->packet_type, offset, header->flags,
                     le16toh(header->checksum));
            if (esp_rcp_fail_corrupt_h2e(header, "zero H2E payload"))
                goto done;
            break;
        }
        if (!ESP_OFFSET_VALID(offset)) {
            H2E_STATS_INC(h2e_drop_invalid);
            ESP_LOGE(TAG, "H2E_AGGR_ERROR: invalid offset pos=%u total=%u "
                     "len=%u offset=%u if=%u type=%u",
                     pos, buf_handle->payload_len, payload_len, offset,
                     header->if_type, header->packet_type);
            if (esp_rcp_fail_corrupt_h2e(header, "invalid H2E offset"))
                goto done;
            break;
        }

        frame_len = payload_len + offset;
        aligned_len = (frame_len + 3) & ~3;
        if (frame_len > RX_BUF_SIZE || pos + frame_len > buf_handle->payload_len) {
            H2E_STATS_INC(h2e_drop_invalid);
            ESP_LOGE(TAG, "H2E_AGGR_ERROR: frame out of bounds pos=%u "
                     "total=%u len=%u offset=%u frame=%u max=%u if=%u type=%u",
                     pos, buf_handle->payload_len, payload_len, offset,
                     frame_len, RX_BUF_SIZE, header->if_type,
                     header->packet_type);
            if (esp_rcp_fail_corrupt_h2e(header, "H2E frame out of bounds"))
                goto done;
            break;
        }

#if CONFIG_ESP_SDIO_CHECKSUM
        rx_checksum = le16toh(header->checksum);
        header->checksum = 0;
        checksum = compute_checksum(buf_handle->payload + pos, frame_len);
        header->checksum = htole16(rx_checksum);
        if (checksum != rx_checksum) {
            H2E_STATS_INC(h2e_drop_checksum);
            ESP_LOGE(TAG, "H2E_CHECKSUM_ERROR: pos=%u total=%u len=%u "
                     "offset=%u if=%u type=%u expected=0x%04x got=0x%04x",
                     pos, buf_handle->payload_len, payload_len, offset,
                     header->if_type, header->packet_type, checksum,
                     rx_checksum);
            if (esp_rcp_fail_corrupt_h2e(header, "H2E checksum mismatch"))
                goto done;
            break;
        }
#endif

        payload = buf_handle->payload + pos + offset;
        H2E_STATS_INC(h2e_rx_pkts);
        H2E_STATS_ADD(h2e_rx_bytes, payload_len);
#if CONFIG_ESP_WLAN_DEBUG
        ESP_LOG_BUFFER_HEXDUMP(TAG_RX, payload, 8, ESP_LOG_INFO);
#endif
        /*ESP_LOG_BUFFER_HEXDUMP("SDIO Rx", payload, payload_len, ESP_LOG_INFO);*/

#ifdef CONFIG_ESP_HOSTED_RCP
        if (header->if_type == ESP_RCP_IF) {
            if (header->packet_type == PACKET_TYPE_RCP_SESSION) {
                if (payload_len == sizeof(struct esp_rcp_session_marker)) {
                    const struct esp_rcp_session_marker *marker =
                        (const struct esp_rcp_session_marker *)payload;
                    uint64_t nonce_le;
                    esp_err_t session_ret;

                    memcpy(&nonce_le, &marker->nonce, sizeof(nonce_le));
                    session_ret = esp_hosted_rcp_session_start_from_host(
                        marker->version, le64toh(nonce_le));
                    if (session_ret != ESP_OK) {
                        ESP_LOGE(TAG, "RCP session marker processing failed: %s",
                                 esp_err_to_name(session_ret));
                        esp_hosted_rcp_fatal_restart(
                            "H2E RCP session marker processing failed");
                        goto done;
                    }
                } else {
                    ESP_LOGW(TAG, "Ignoring stale RCP session marker len=%u",
                             payload_len);
                }
            } else if (header->packet_type != PACKET_TYPE_DATA) {
                ESP_LOGE(TAG, "Invalid RCP packet type=%u",
                         header->packet_type);
                esp_hosted_rcp_fatal_restart("invalid H2E RCP packet type");
                goto done;
            } else if (esp_hosted_rcp_rx_from_host(payload, payload_len) != ESP_OK) {
                ESP_LOGE(TAG, "RCP RX queue overflow/unavailable");
                esp_hosted_rcp_fatal_restart("H2E RCP queue overflow/unavailable");
                goto done;
            }
        } else
#endif
        if (header->packet_type == PACKET_TYPE_COMMAND_REQUEST) {
            /* Process command Request */
            /*ESP_LOG_BUFFER_HEXDUMP("Rx Cmd", payload, payload_len, ESP_LOG_INFO);*/
            process_priv_commamd(header->if_type, payload, payload_len);

        } else if (header->packet_type == PACKET_TYPE_DATA ||
                   header->packet_type == PACKET_TYPE_EAPOL) {

            /* ESP_LOGI(TAG, "Data packet on iface=%d", header->if_type); */
            /* Data Path */
            if (header->if_type == ESP_STA_IF) {
                /*ESP_LOGI(TAG, "Station IF");*/

                /* Forward packet over station interface */
                if ((station_connected || association_ongoing) &&
                    (station_authorized || header->packet_type == PACKET_TYPE_EAPOL)) {
                    int ret = 0;
                    /*ESP_LOGI(TAG, "Send wlan\n");*/
                    ret = wifi_tx_with_retry(WIFI_IF_STA, payload, payload_len);
                    if (ret) {
                        H2E_STATS_INC(h2e_wifi_tx_fail);
                        ESP_LOGW(TAG, "STA data tx failed=%d", ret);
                    } else {
                        H2E_STATS_INC(h2e_wifi_tx_ok);
                    }
                } else {
                    H2E_STATS_INC(h2e_drop_not_ready);
                    ESP_LOGD(TAG, "H2E_DROP_READY pkt=%u connected=%u assoc=%u authorized=%u",
                             header->packet_type, station_connected,
                             association_ongoing, station_authorized);
                }

            } else if (header->if_type == ESP_AP_IF && softap_started) {

                /* Forward packet over soft AP interface */
                /* ESP_LOGI(TAG, "Send data pkt over wlan\n"); */
                int ret = wifi_tx_with_retry(WIFI_IF_AP, payload, payload_len);
                if (ret) {
                    H2E_STATS_INC(h2e_wifi_tx_fail);
                    ESP_LOGW(TAG, "Sending data failed=%d\n", ret);
                } else {
                    H2E_STATS_INC(h2e_wifi_tx_ok);
                }
            }
#if defined(CONFIG_BT_ENABLED) && BLUETOOTH_HCI
            else if (header->if_type == ESP_HCI_IF) {
                /*
                 * Backward compatibility: pre-runtime hosts create hciX from
                 * the legacy BT capability bits and immediately send HCI.
                 * If no runtime-selection command has been seen, activate BT
                 * lazily on that first packet. New hosts always send SET
                 * RADIO_SERVICE (including "none"), so they retain default-off.
                 */
                if (!(s_radio_service & ESP_RADIO_SERVICE_BT) &&
                    !s_radio_service_command_seen) {
                    esp_err_t bt_ret = initialise_bluetooth();

                    if (bt_ret == ESP_OK) {
                        s_radio_service |= ESP_RADIO_SERVICE_BT;
                        ESP_LOGI(TAG, "Legacy host detected; Bluetooth activated lazily");
                    } else {
                        ESP_LOGE(TAG, "Legacy BT activation failed: %s",
                                 esp_err_to_name(bt_ret));
                    }
                }
                if (s_radio_service & ESP_RADIO_SERVICE_BT)
                    process_hci_rx_pkt(payload, payload_len);
                else
                    ESP_LOGW(TAG, "Dropping HCI while radio service mask=0x%02x",
                             s_radio_service);
            }
#endif
            else if (header->if_type == ESP_TEST_IF) {
                debug_update_raw_tp_rx_count(header, payload_len);
            }
        }

        pos += aligned_len;
    }
    print_h2e_stats();

done:
    /* Free buffer handle */
    if (buf_handle->free_buf_handle && buf_handle->priv_buffer_handle) {
        buf_handle->free_buf_handle(buf_handle->priv_buffer_handle);
        buf_handle->priv_buffer_handle = NULL;
    }
}

/* Get data from host */
void recv_task(void* pvParameters)
{
    interface_buffer_handle_t buf_handle;

    for (;;) {

        if (!datapath) {
            /* Datapath is not enabled by host yet*/
            usleep(100 * 1000);
            continue;
        }

        /* receive data from transport layer */
        if (if_context && if_context->if_ops && if_context->if_ops->read) {
            int len = if_context->if_ops->read(if_handle, &buf_handle);
            if (len <= 0) {
                usleep(10 * 1000);
                continue;
            }
        }

        process_rx_pkt(&buf_handle);
    }
}

int event_handler(uint8_t val)
{
    switch (val) {

    case ESP_OPEN_DATA_PATH:

        if (if_handle) {
            if_handle->state = ACTIVE;
            datapath = 1;
            ESP_EARLY_LOGI(TAG, "Start Data Path");

            if (init_sem) {
                xSemaphoreGive(init_sem);
            }
        } else {
            ESP_EARLY_LOGI(TAG, "Failed to Start Data Path");
        }
        break;

    case ESP_CLOSE_DATA_PATH:

        datapath = 0;
        if (if_handle) {
            ESP_EARLY_LOGI(TAG, "Stop Data Path");
            if_handle->state = DEACTIVE;
        } else {
            ESP_EARLY_LOGI(TAG, "Failed to Stop Data Path");
        }
        esp_restart();
        break;

    case ESP_POWER_SAVE_ON:
        ESP_EARLY_LOGI(TAG, "Host Sleep");
        if (wakeup_sem) {
            /* Host sleeping */
            xSemaphoreTake(wakeup_sem, portMAX_DELAY);
        }
        power_save_on = 1;

        if_handle->state = ACTIVE;
        break;

    case ESP_POWER_SAVE_OFF:
        ESP_EARLY_LOGI(TAG, "Host Awake");
        if_handle->state = ACTIVE;
        power_save_on = 0;
        if (wakeup_sem) {
            xSemaphoreGive(wakeup_sem);
        }
        break;

    }
    return 0;
}

static void set_gpio_cd_pin(void)
{
#if CONFIG_SDIO_CARD_DETECTION_PIN_SUPPORT
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << CONFIG_SDIO_CD_PIN_GPIO);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    gpio_set_level(CONFIG_SDIO_CD_PIN_GPIO, 1);
#endif
}

void app_main()
{
    esp_err_t ret;
    uint8_t prio_q_idx = 0;
    uint32_t capa = 0;
    uint32_t ext_capa = 0;
#ifdef CONFIG_BT_ENABLED
    uint8_t mac[MAC_ADDR_LEN] = {0};
#endif
    debug_log_firmware_version();

    capa = get_capabilities();

    /*Initialize NVS*/
    ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = initialise_wifi();
    ESP_ERROR_CHECK(ret);

#ifdef CONFIG_ESP_THREAD_RCP_UART
    /*
     * Dedicated Thread RCP is independent of the ESP-Hosted data path and
     * starts at boot, just like a standalone RCP image.
     */
    ret = esp_thread_rcp_uart_start();
    ESP_ERROR_CHECK(ret);
#endif

    init_sem = xSemaphoreCreateBinary();
    if (init_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create init semaphore\n");
        return;
    }

#ifdef CONFIG_BT_ENABLED
#ifdef BLUETOOTH_UART
    /*
     * Preserve the legacy independent HCI-over-UART product mode. There is no
     * ESP-Hosted HCI endpoint in this configuration, so the controller must be
     * available without a Hosted radio-service command.
     */
    ret = initialise_bluetooth();
    ESP_ERROR_CHECK(ret);
#endif

    /* Hosted HCI stays runtime-selectable; UART HCI is already active above. */
    ret = esp_read_mac(mac, ESP_MAC_BT);
    if (ret) {
        ESP_LOGE(TAG, "Failed to read BT Mac addr\n");
    } else {
        ESP_LOGI(TAG, "ESP Bluetooth MAC addr: %2x-%2x-%2x-%2x-%2x-%2x\n",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
#endif

    if_context = interface_insert_driver(event_handler);
#if CONFIG_ESP_SPI_HOST_INTERFACE || CONFIG_ESP_USB_HOST_INTERFACE
    datapath = 1;
#endif

    if (!if_context || !if_context->if_ops) {
        ESP_LOGE(TAG, "Failed to insert driver\n");
        return;
    }

    if_handle = if_context->if_ops->init();

    if (!if_handle) {
        ESP_LOGE(TAG, "Failed to initialize driver\n");
        return;
    }

#if !CONFIG_ESP_SPI_HOST_INTERFACE
    /* SPI boot readiness is synchronized in hardware via spi_post_setup_cb(),
     * which asserts HANDSHAKE and DATA_READY only after the boot-up event DMA
     * descriptor is armed. Keep the legacy startup delay scoped to non-SPI
     * transports. */
    sleep(1);
#endif

    for (prio_q_idx = 0; prio_q_idx < MAX_PRIORITY_QUEUES; prio_q_idx++) {
        to_host_queue[prio_q_idx] = xQueueCreate(TO_HOST_QUEUE_SIZE, sizeof(interface_buffer_handle_t));
        assert(to_host_queue[prio_q_idx] != NULL);
    }

    assert(xTaskCreate(recv_task, "recv_task", TASK_DEFAULT_STACK_SIZE, NULL, TASK_DEFAULT_PRIO, NULL) == pdTRUE);
    assert(xTaskCreate(send_task, "send_task", TASK_DEFAULT_STACK_SIZE, NULL, TASK_DEFAULT_PRIO, &send_task_handle) == pdTRUE);

    create_debugging_tasks();

    set_gpio_cd_pin();

    /* Advertise support only. The host selects the secondary radio at runtime. */
    ext_capa = get_extended_capabilities();

    /* send capabilities to host */
    if (datapath || xSemaphoreTake(init_sem, portMAX_DELAY)) {
        ret = send_bootup_event_to_host(capa, ext_capa);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "Failed to publish boot event: %s", esp_err_to_name(ret));
    }

    debug_set_wifi_logging();
    ESP_LOGI(TAG, "Initial set up done");
}
