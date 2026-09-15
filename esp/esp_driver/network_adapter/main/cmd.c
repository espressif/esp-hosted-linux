/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <inttypes.h>
#include "esp_log.h"
#include "interface.h"
#include "stats.h"
#include "esp.h"
#include "cmd.h"
#include "adapter.h"
#include "endian.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_private/wifi.h"
#include "esp_wpa.h"
#include "app_main.h"
#include "esp_wifi.h"
#include "esp_wifi_driver.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_check.h"
#include "wifi_defs.h"
#include <time.h>
#include <sys/time.h>
#include "esp_ota_ops.h"
#include "esp_app_format.h"

#define TAG "FW_CMD"

static bool verify_ota = false;
static uint8_t broadcast_mac[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
#define IS_BROADCAST_ADDR(addr) (memcmp(addr, broadcast_mac, ETH_ALEN) == 0)

/* This is limitation of ESP WiFi lib.
 * It needs the password field updated to understand
 * we are triggering non-open connection */
#define DUMMY_PASSPHRASE            "12345678"

#define RESET_TIMEOUT             (5*1000)
extern volatile uint8_t station_connected;
extern volatile uint8_t station_authorized;
static uint8_t s_assoc_control_port = 0;
static uint8_t s_assoc_is_reassoc = 0;
extern volatile uint8_t association_ongoing;
extern volatile uint8_t softap_started;
static const uint8_t zero_mac[MAC_ADDR_LEN] = {0};

#define HOSTED_MGMT_TX_ASYNC_STATUS_V1 0xA5
#define HOSTED_EVENT_MGMT_TX_STATUS    7
#define HOSTED_MGMT_STATUS_TIMEOUT_MS  5000
#define HOSTED_MGMT_HDR_LEN            24U
#define HOSTED_MGMT_BODY_MATCH_LEN     16U

struct hosted_mgmt_tx_status_event {
    struct event_header header;
    uint64_t cookie;
    uint32_t frame_len;
    uint8_t ack;
    uint8_t pad[3];
    uint8_t frame[];
} __packed;

struct hosted_mgmt_status_state {
    bool pending;
    bool accepted;
    bool completed;
    bool ack;
    bool report_status;
    uint64_t cookie;
    uint8_t *frame;
    uint32_t frame_len;
    TimerHandle_t timer;
};

static struct hosted_mgmt_status_state s_hosted_mgmt;
static portMUX_TYPE s_hosted_mgmt_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_hosted_mgmt_draining;

static int prepare_event(uint8_t if_type,
                         interface_buffer_handle_t *buf_handle,
                         uint16_t event_len);
static void mgmt_txcb(void *eb);
static void mgmt_txcb_unmarked(void *eb);

static uint16_t s_assoc_generation;
static uint8_t s_assoc_bssid[MAC_ADDR_LEN];
static uint16_t s_auth_generation;
static uint8_t s_auth_bssid[MAC_ADDR_LEN];
static uint16_t s_conn_generation;
static uint16_t s_disconnect_generation;
static uint8_t s_disconnect_subtype;

volatile uint8_t sta_init_flag;

/* EAP and IEEE 802.1X constants */
#define IEEE802_1X_VERSION                  2
#define IEEE802_1X_TYPE_EAP_PACKET          0
#define EAP_CODE_REQUEST                    1
#define EAP_CODE_RESPONSE                   2
#define EAP_CODE_SUCCESS                    3
#define EAP_CODE_FAILURE                    4

static struct wpa_funcs wpa_cb;
static struct wpa2_funcs *wpa2_cb;
/* The host permits only one generic command in flight, so synchronous command
 * responses can use this active request context. */
volatile uint16_t active_cmd_seq;
volatile uint8_t active_cmd_code;
/* One async management TX at a time. The completion callback must stamp the
 * sequence captured at submit, not whatever request arrived later. */
static portMUX_TYPE s_mgmt_tx_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_mgmt_tx_pending;
static bool s_mgmt_cb_owned;
static bool s_wifi_started;
static uint16_t s_mgmt_tx_seq;
static uint8_t s_mgmt_tx_da[ETH_ALEN];
static bool s_mgmt_tx_token_valid;
static esp_event_handler_instance_t instance_any_id;
static uint8_t *ap_bssid;

extern uint32_t ip_address;
extern struct macfilter_list mac_list;
extern uint8_t dev_mac[MAC_ADDR_LEN];

uint8_t dummy_mac[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
uint8_t dummy_mac2[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x66};

int esp_wifi_register_wpa_cb_internal(struct wpa_funcs *cb);
int esp_wifi_unregister_wpa_cb_internal(void);
/* libpp: real slot occupancy status, unlike register_tx_cb_internal(). */
int pp_unregister_tx_cb(uint8_t id);
int pp_register_tx_cb(void *fn, uint8_t id, uint8_t flag);
extern esp_err_t wlan_sta_rx_callback(void *buffer, uint16_t len, void *eb);
extern volatile uint8_t power_save_on;
extern void wake_host();
extern struct wow_config wow;

#if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
#define TX_DONE_PREFIX 8
#else
#define TX_DONE_PREFIX 0
#endif
static const esp_partition_t* update_partition = NULL;
static esp_ota_handle_t handle;

extern int wpa_parse_wpa_ie_wrapper(const u8 *wpa_ie, size_t wpa_ie_len, wifi_wpa_ie_t *data);
static inline void WPA_PUT_LE16(u8 *a, u16 val)
{
    a[1] = val >> 8;
    a[0] = val & 0xff;
}

static bool cmd_flex_len_valid(uint16_t payload_len, size_t fixed_len,
                               size_t embedded_len)
{
    return fixed_len <= (size_t)payload_len &&
           embedded_len <= (size_t)payload_len - fixed_len;
}

static bool mgmt_tx_hdr_matches(const uint8_t *frame, uint32_t len)
{
    if (!frame || len < (ETH_ALEN + 4))
        return false;
    /* ieee80211_send_mgmt_internal() overwrites 802.11 seqctl (bytes 22-23)
     * with the chip's counter. Match DA only. Stop-flush plus one-inflight
     * pending prevent a late callback from completing a newer request. */
    return memcmp(frame + 4, s_mgmt_tx_da, ETH_ALEN) == 0;
}

static void mgmt_tx_store_token(const uint8_t *frame, uint32_t len)
{
    s_mgmt_tx_token_valid = false;
    memset(s_mgmt_tx_da, 0, ETH_ALEN);
    if (!frame || len < (ETH_ALEN + 4))
        return;
    memcpy(s_mgmt_tx_da, frame + 4, ETH_ALEN);
    s_mgmt_tx_token_valid = true;
}

static bool mgmt_tx_token_matches(const uint8_t *data, uint32_t len)
{
    if (!s_mgmt_tx_token_valid || !data)
        return false;
    /* HE chips prefix TX-done blobs; non-HE do not. Try both layouts so a
     * compile-time PREFIX mismatch cannot leak the inflight slot forever. */
    if (mgmt_tx_hdr_matches(data, len))
        return true;
    if (len > 8 && mgmt_tx_hdr_matches(data + 8, len - 8))
        return true;
    return false;
}

static int send_mgmt_tx_done(uint8_t cmd_status, wifi_interface_t wifi_if_type,
                             uint8_t *data, uint32_t len, uint16_t seq);
static bool mgmt_tx_inflight_take(uint16_t *seq);

static bool mgmt_tx_inflight_begin(uint16_t seq, const uint8_t *frame, uint32_t len)
{
    bool ok;

    portENTER_CRITICAL(&s_mgmt_tx_lock);
    ok = !s_mgmt_tx_pending;
    if (ok) {
        s_mgmt_tx_pending = true;
        s_mgmt_tx_seq = seq;
        mgmt_tx_store_token(frame, len);
    }
    portEXIT_CRITICAL(&s_mgmt_tx_lock);
    return ok;
}

static bool mgmt_tx_inflight_take(uint16_t *seq)
{
    bool had;

    portENTER_CRITICAL(&s_mgmt_tx_lock);
    had = s_mgmt_tx_pending;
    if (had && seq)
        *seq = s_mgmt_tx_seq;
    s_mgmt_tx_pending = false;
    s_mgmt_tx_token_valid = false;
    portEXIT_CRITICAL(&s_mgmt_tx_lock);
    return had;
}

static bool mgmt_tx_inflight_take_matching(const uint8_t *data, uint32_t len,
                                           uint16_t *seq)
{
    bool had = false;

    portENTER_CRITICAL(&s_mgmt_tx_lock);
    if (s_mgmt_tx_pending && mgmt_tx_token_matches(data, len)) {
        had = true;
        if (seq)
            *seq = s_mgmt_tx_seq;
        s_mgmt_tx_pending = false;
        s_mgmt_tx_token_valid = false;
    }
    portEXIT_CRITICAL(&s_mgmt_tx_lock);
    return had;
}

static bool hosted_mgmt_frame_matches_locked(const uint8_t *candidate,
                                             uint32_t candidate_len)
{
    const uint8_t *reference = s_hosted_mgmt.frame;
    uint32_t body_len;

    if (!s_hosted_mgmt.pending || !reference || !candidate ||
        s_hosted_mgmt.frame_len < HOSTED_MGMT_HDR_LEN ||
        candidate_len < HOSTED_MGMT_HDR_LEN)
        return false;

    /* Match type/subtype plus DA/SA/BSSID and a stable body prefix. Duration
     * (2..3) and sequence-control (22..23) may be rewritten by Wi-Fi HW. */
    if (candidate[0] != reference[0] ||
        memcmp(candidate + 4, reference + 4, 18) != 0)
        return false;

    body_len = s_hosted_mgmt.frame_len - HOSTED_MGMT_HDR_LEN;
    if (body_len > HOSTED_MGMT_BODY_MATCH_LEN)
        body_len = HOSTED_MGMT_BODY_MATCH_LEN;
    if (candidate_len < HOSTED_MGMT_HDR_LEN + body_len)
        return false;
    if (body_len && memcmp(candidate + HOSTED_MGMT_HDR_LEN,
                           reference + HOSTED_MGMT_HDR_LEN, body_len) != 0)
        return false;

    return true;
}

static bool hosted_mgmt_data_matches_locked(const uint8_t *data, uint32_t len)
{
    if (hosted_mgmt_frame_matches_locked(data, len))
        return true;
    if (len > 8 && hosted_mgmt_frame_matches_locked(data + 8, len - 8))
        return true;
    return false;
}

static bool hosted_mgmt_take(uint64_t *cookie, uint8_t **frame,
                             uint32_t *frame_len, bool *ack,
                             bool *report_status, TimerHandle_t *timer)
{
    bool had;

    portENTER_CRITICAL(&s_hosted_mgmt_lock);
    had = s_hosted_mgmt.pending;
    if (had) {
        if (cookie)
            *cookie = s_hosted_mgmt.cookie;
        if (frame)
            *frame = s_hosted_mgmt.frame;
        if (frame_len)
            *frame_len = s_hosted_mgmt.frame_len;
        if (ack)
            *ack = s_hosted_mgmt.ack;
        if (report_status)
            *report_status = s_hosted_mgmt.report_status;
        if (timer)
            *timer = s_hosted_mgmt.timer;
        memset(&s_hosted_mgmt, 0, sizeof(s_hosted_mgmt));
    }
    portEXIT_CRITICAL(&s_hosted_mgmt_lock);
    return had;
}

static bool hosted_mgmt_take_timeout(uint64_t *cookie, uint8_t **frame,
                                     uint32_t *frame_len, bool *ack,
                                     bool *report_status, TimerHandle_t *timer)
{
    bool had;

    portENTER_CRITICAL(&s_hosted_mgmt_lock);
    had = s_hosted_mgmt.pending;
    if (had) {
        if (cookie)
            *cookie = s_hosted_mgmt.cookie;
        if (frame)
            *frame = s_hosted_mgmt.frame;
        if (frame_len)
            *frame_len = s_hosted_mgmt.frame_len;
        if (ack)
            *ack = s_hosted_mgmt.ack;
        if (report_status)
            *report_status = s_hosted_mgmt.report_status;
        if (timer)
            *timer = s_hosted_mgmt.timer;
        s_hosted_mgmt_draining = true;
        memset(&s_hosted_mgmt, 0, sizeof(s_hosted_mgmt));
    }
    portEXIT_CRITICAL(&s_hosted_mgmt_lock);
    return had;
}

static int hosted_send_mgmt_accept(uint8_t status, uint16_t seq)
{
    interface_buffer_handle_t buf_handle = {0};
    struct cmd_mgmt_tx *resp;
    int ret;

    buf_handle.if_type = ESP_AP_IF;
    buf_handle.if_num = 0;
    buf_handle.payload_len = sizeof(struct cmd_mgmt_tx);
    buf_handle.pkt_type = PACKET_TYPE_COMMAND_RESPONSE;
    buf_handle.payload = heap_caps_malloc(buf_handle.payload_len, MALLOC_CAP_DMA);
    if (!buf_handle.payload)
        return ESP_ERR_NO_MEM;
    memset(buf_handle.payload, 0, buf_handle.payload_len);

    resp = (struct cmd_mgmt_tx *)buf_handle.payload;
    resp->header.cmd_code = CMD_MGMT_TX;
    resp->header.cmd_status = status;
    resp->header.len = 0;
    resp->header.seq_num = htole16(seq);
    resp->header.reserved1 = HOSTED_MGMT_TX_ASYNC_STATUS_V1;
    resp->len = htole32(0);

    buf_handle.priv_buffer_handle = buf_handle.payload;
    buf_handle.free_buf_handle = free;
    ret = send_command_response(&buf_handle);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "MGMT acceptance response failed seq=%u", seq);
        free(buf_handle.payload);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static int hosted_send_mgmt_status(bool ack, const uint8_t *frame,
                                   uint32_t frame_len, uint64_t cookie)
{
    interface_buffer_handle_t buf_handle = {0};
    struct hosted_mgmt_tx_status_event *event;
    esp_err_t ret;

    if (!cookie || !frame || !frame_len ||
        frame_len > UINT16_MAX - sizeof(struct hosted_mgmt_tx_status_event))
        return ESP_ERR_INVALID_ARG;

    ret = prepare_event(ESP_AP_IF, &buf_handle,
                        sizeof(struct hosted_mgmt_tx_status_event) + frame_len);
    if (ret)
        return ret;

    event = (struct hosted_mgmt_tx_status_event *)buf_handle.payload;
    event->header.event_code = HOSTED_EVENT_MGMT_TX_STATUS;
    event->header.status = 0;
    event->header.len = htole16(buf_handle.payload_len -
                                sizeof(struct event_header));
    event->cookie = htole64(cookie);
    event->frame_len = htole32(frame_len);
    event->ack = ack ? 1 : 0;
    memcpy(event->frame, frame, frame_len);

    ret = send_command_event(&buf_handle);
    if (ret != pdTRUE) {
        vTaskDelay(pdMS_TO_TICKS(10));
        ret = send_command_event(&buf_handle);
    }
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "MGMT status event send failed cookie=%"PRIu64, cookie);
        free(buf_handle.payload);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void hosted_mgmt_finish_owned(uint64_t cookie, uint8_t *frame,
                                     uint32_t frame_len, bool ack,
                                     bool report_status, TimerHandle_t timer)
{
    esp_err_t ret = ESP_OK;

    if (timer) {
        (void)xTimerStop(timer, 0);
        (void)xTimerDelete(timer, 0);
    }
    if (report_status)
        ret = hosted_send_mgmt_status(ack, frame, frame_len, cookie);
    free(frame);

    if (report_status && ret != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: failed to deliver MGMT status to host (cookie=%"PRIu64", err=%d), restarting",
                 cookie, ret);
        esp_restart();
    }
}

static void hosted_mgmt_timeout_cb(TimerHandle_t timer)
{
    uint64_t cookie = 0;
    uint8_t *frame = NULL;
    uint32_t frame_len = 0;
    bool ack = false;
    bool report_status = false;
    TimerHandle_t owned_timer = NULL;

    /* The timer is armed only after acceptance has been queued. State/timer
     * ownership transfers atomically; if another path already took it, that
     * path is the sole owner and this callback must not touch the timer.
     * Taking the timed-out transaction and setting s_hosted_mgmt_draining = true
     * occurs in the exact same critical section to eliminate preemption window. */
    if (!hosted_mgmt_take_timeout(&cookie, &frame, &frame_len, &ack,
                                  &report_status, &owned_timer))
        return;

    hosted_mgmt_finish_owned(cookie, frame, frame_len, false,
                             report_status, owned_timer);

    ESP_LOGE(TAG, "Marked MGMT TX timed out (cookie=%"PRIu64"); restarting firmware to guarantee clean Wi-Fi state", cookie);
    esp_restart();
}

static int hosted_mgmt_begin(uint64_t cookie, const uint8_t *frame,
                             uint32_t frame_len, bool report_status)
{
    uint8_t *copy;
    TimerHandle_t timer;
    bool busy;

    copy = malloc(frame_len);
    if (!copy)
        return ESP_ERR_NO_MEM;
    memcpy(copy, frame, frame_len);

    timer = xTimerCreate("mgmt-status",
                         pdMS_TO_TICKS(HOSTED_MGMT_STATUS_TIMEOUT_MS),
                         pdFALSE, NULL, hosted_mgmt_timeout_cb);
    if (!timer) {
        free(copy);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_hosted_mgmt_lock);
    busy = s_hosted_mgmt.pending || s_hosted_mgmt_draining;
    if (!busy) {
        memset(&s_hosted_mgmt, 0, sizeof(s_hosted_mgmt));
        s_hosted_mgmt.pending = true;
        s_hosted_mgmt.cookie = cookie;
        s_hosted_mgmt.frame = copy;
        s_hosted_mgmt.frame_len = frame_len;
        s_hosted_mgmt.report_status = report_status;
        s_hosted_mgmt.timer = timer;
    }
    portEXIT_CRITICAL(&s_hosted_mgmt_lock);

    if (busy) {
        (void)xTimerDelete(timer, 0);
        free(copy);
        return ESP_ERR_INVALID_STATE;
    }

    /* Do not start the timeout yet. send_command_response() may block while
     * the high-priority host queue is full; starting here could expire and
     * destroy the state before a successful acceptance is ever observable. */
    return ESP_OK;
}

static void hosted_mgmt_abort_pending(void)
{
    uint64_t cookie;
    uint8_t *frame;
    uint32_t frame_len;
    bool ack, report_status;
    TimerHandle_t timer;

    portENTER_CRITICAL(&s_hosted_mgmt_lock);
    s_hosted_mgmt_draining = false;
    portEXIT_CRITICAL(&s_hosted_mgmt_lock);

    if (!hosted_mgmt_take(&cookie, &frame, &frame_len, &ack,
                          &report_status, &timer))
        return;
    hosted_mgmt_finish_owned(cookie, frame, frame_len, false, false, timer);
}

static void hosted_mgmt_fail_pending(void)
{
    uint64_t cookie;
    uint8_t *frame;
    uint32_t frame_len;
    bool ack, report_status;
    TimerHandle_t timer;

    portENTER_CRITICAL(&s_hosted_mgmt_lock);
    s_hosted_mgmt_draining = false;
    portEXIT_CRITICAL(&s_hosted_mgmt_lock);

    if (!hosted_mgmt_take(&cookie, &frame, &frame_len, &ack,
                          &report_status, &timer))
        return;
    hosted_mgmt_finish_owned(cookie, frame, frame_len, false,
                             report_status, timer);
}

static void hosted_mgmt_mark_accepted(uint64_t cookie)
{
    uint8_t *frame = NULL;
    uint32_t frame_len = 0;
    bool ack = false;
    bool report_status = false;
    TimerHandle_t timer = NULL;
    TimerHandle_t start_timer = NULL;
    bool complete = false;

    /* While accepted is false the TX callback records completion but never
     * takes state, so it is safe to obtain and arm this timer before flipping
     * accepted. No later host command can execute until this handler returns. */
    portENTER_CRITICAL(&s_hosted_mgmt_lock);
    if (s_hosted_mgmt.pending && s_hosted_mgmt.cookie == cookie)
        start_timer = s_hosted_mgmt.timer;
    portEXIT_CRITICAL(&s_hosted_mgmt_lock);

    if (!start_timer || xTimerStart(start_timer, 0) != pdPASS) {
        if (hosted_mgmt_take(&cookie, &frame, &frame_len, &ack,
                             &report_status, &timer))
            hosted_mgmt_finish_owned(cookie, frame, frame_len, false,
                                     report_status, timer);
        return;
    }

    portENTER_CRITICAL(&s_hosted_mgmt_lock);
    if (s_hosted_mgmt.pending && s_hosted_mgmt.cookie == cookie) {
        s_hosted_mgmt.accepted = true;
        complete = s_hosted_mgmt.completed;
        if (complete) {
            frame = s_hosted_mgmt.frame;
            frame_len = s_hosted_mgmt.frame_len;
            ack = s_hosted_mgmt.ack;
            report_status = s_hosted_mgmt.report_status;
            timer = s_hosted_mgmt.timer;
            memset(&s_hosted_mgmt, 0, sizeof(s_hosted_mgmt));
        }
    }
    portEXIT_CRITICAL(&s_hosted_mgmt_lock);

    if (complete)
        hosted_mgmt_finish_owned(cookie, frame, frame_len, ack,
                                 report_status, timer);
}


#define WIFI_TXCB_MGMT_ID 2

static int mgmt_tx_ensure_cb(void)
{
    int ret;

    if (s_mgmt_cb_owned)
        return ESP_OK;

    /* Slot 2 starts as ieee80211_tx_mgt_cb. ppRegisterTxCallback refuses an
     * occupied slot and stores NULL if the slot was empty; the
     * register_tx_cb_internal wrapper still returns 0. Unregister first. */
    (void)pp_unregister_tx_cb(WIFI_TXCB_MGMT_ID);

    ret = pp_register_tx_cb(mgmt_txcb, WIFI_TXCB_MGMT_ID, 1);
    if (ret) {
        s_mgmt_cb_owned = false;
        ESP_LOGE(TAG, "mgmt tx cb register ret=%d", ret);
        return ret;
    }
    s_mgmt_cb_owned = true;
    return ESP_OK;
}

static void mgmt_tx_fail_pending(void)
{
    uint16_t seq;

    if (!mgmt_tx_inflight_take(&seq))
        return;
    send_mgmt_tx_done(CMD_RESPONSE_FAIL, WIFI_IF_AP, NULL, 0, seq);
}

static esp_err_t hosted_wifi_stop(void)
{
    esp_err_t ret = esp_wifi_stop();

    if (ret == ESP_OK) {
        s_wifi_started = false;
        /* esp_wifi_stop() cancels in-flight TX and often never invokes the
         * TX-done callback. Fail the host waiter and free the slot. */
        s_mgmt_cb_owned = false;
        mgmt_tx_fail_pending();
    } else {
        ESP_LOGE(TAG, "esp_wifi_stop failed ret=%d", ret);
    }
    return ret;
}

static esp_err_t hosted_wifi_start(void)
{
    esp_err_t ret = esp_wifi_start();

    if (ret == ESP_OK) {
        s_wifi_started = true;
        /* esp_wifi_start() can restore the library's default MGMT TX cb. */
        s_mgmt_cb_owned = false;
        ret = mgmt_tx_ensure_cb();
        if (ret) {
            ESP_LOGE(TAG, "mgmt tx cb install after wifi_start ret=%d", ret);
            (void)hosted_wifi_stop();
            return ret;
        }
    }
    return ret;
}

static void esp_wifi_set_debug_log()
{
    /* set WiFi log level and module */
    uint32_t g_wifi_log_module = WIFI_LOG_MODULE_WIFI;
    uint32_t g_wifi_log_submodule = 0;

    g_wifi_log_submodule |= WIFI_LOG_SUBMODULE_ALL;
    g_wifi_log_submodule |= WIFI_LOG_SUBMODULE_INIT;
    g_wifi_log_submodule |= WIFI_LOG_SUBMODULE_IOCTL;
    g_wifi_log_submodule |= WIFI_LOG_SUBMODULE_CONN;
    g_wifi_log_submodule |= WIFI_LOG_SUBMODULE_SCAN;

    esp_wifi_internal_set_log_mod(g_wifi_log_module, g_wifi_log_submodule, true);

    esp_wifi_internal_set_log_level(WIFI_LOG_VERBOSE);
}

static int send_command_resp(uint8_t if_type,
                             uint8_t cmd_code, uint8_t cmd_status,
                             uint8_t *data, uint32_t len)
{
    int ret;
    struct command_header *header;
    interface_buffer_handle_t buf_handle = {0};

    ESP_LOGD(TAG, "CMD_RESP_QUEUE code=%u seq=%u status=%u len=%"PRIu32,
             cmd_code, active_cmd_seq, cmd_status, len);
    if (active_cmd_code != cmd_code) {
        ESP_LOGE(TAG, "CMD_RESP_CONTEXT_MISMATCH active=%u/%u response=%u",
                 active_cmd_code, active_cmd_seq, cmd_code);
    }

    buf_handle.payload_len = sizeof(struct command_header) +
                             sizeof(struct esp_payload_header) +
                             len;
    buf_handle.payload = heap_caps_malloc(buf_handle.payload_len, MALLOC_CAP_DMA);
    if (!buf_handle.payload) {
        ESP_LOGE(TAG, "Malloc send buffer fail!");
        return ESP_FAIL;
    }
    memset(buf_handle.payload, 0, buf_handle.payload_len);

    header = (struct command_header *) buf_handle.payload;
    header->cmd_status = cmd_status;
    header->cmd_code = cmd_code;
    header->len = htole16(len);
    header->seq_num = htole16(active_cmd_seq);

    if (data && len) {
        memcpy(buf_handle.payload + sizeof(struct command_header), data, len);
    }

    header->cmd_status = cmd_status;
    header->cmd_code = cmd_code;
    header->len = htole16(len);
    header->seq_num = htole16(active_cmd_seq);

    buf_handle.if_type = if_type;
    buf_handle.if_num = 0;
    buf_handle.pkt_type = PACKET_TYPE_COMMAND_RESPONSE;

    buf_handle.priv_buffer_handle = buf_handle.payload;
    buf_handle.free_buf_handle = free;

    /* Send command response */
    ret = send_command_response(&buf_handle);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send command response\n");
        goto cleanup;
    }

    ESP_LOGD(TAG, "CMD_RESP_QUEUED code=%u seq=%u status=%u",
             cmd_code, active_cmd_seq, cmd_status);

    return ESP_OK;
cleanup:
    if (buf_handle.payload) {
        free(buf_handle.payload);
        buf_handle.payload = NULL;
    }

    return ret;
}

static void deinitialize_wifi(void)
{
    /*esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
      &esp_scan_done_event_handler);*/
    hosted_wifi_stop();
    esp_wifi_deinit();
}

static void cleanup_ap_bssid(void)
{
    ESP_LOGI(TAG, "%s", __func__);
    if (ap_bssid) {
        free(ap_bssid);
        ap_bssid = NULL;
    }
}

static void cleanup_wpa2_cb(void)
{
    /* unregister_wpa2_cb_internal() already os_free()s the stored table
     * (g_osi_funcs._free). Do not free it again. */
    esp_wifi_unregister_wpa2_cb_internal();
    wpa2_cb = NULL;
}

bool sta_init(void)
{
    return true;
}

bool sta_deinit(void)
{
    return true;
}

int sta_connection(uint8_t *bssid)
{
    if (!bssid) {
        return ESP_FAIL;
    }

    ap_bssid = (uint8_t*)malloc(MAC_ADDR_LEN);
    if (!ap_bssid) {
        ESP_LOGI(TAG, "%s:%u malloc failed\n", __func__, __LINE__);
        return ESP_FAIL;
    }
    memcpy(ap_bssid, bssid, MAC_ADDR_LEN);
    esp_wifi_sta_connect_internal(ap_bssid);

    return ESP_OK;
}

int station_rx_eapol(uint8_t *src_addr, uint8_t *buf, uint32_t len)
{
    esp_err_t ret = ESP_OK;
    interface_buffer_handle_t buf_handle = {0};
    uint8_t * tx_buf = NULL;
    u8 own_mac[MAC_ADDR_LEN] = {0};

    if (!src_addr || !buf || !len) {
        ESP_LOGI(TAG, "eapol err - src_addr: %p buf: %p len: %lu\n",
                 src_addr, buf, len);
        //TODO : free buf using esp_wifi_internal_free_rx_buffer?
        return ESP_FAIL;
    }

    if (!ap_bssid) {
        ESP_LOGI(TAG, "AP bssid null\n");
        return ESP_FAIL;
    }

    ret = esp_wifi_get_macaddr_internal(0, own_mac);
    if (ret) {
        ESP_LOGI(TAG, "Failed to get own sta MAC addr\n");
        return ESP_FAIL;
    }

#if CONFIG_ESP_SDIO_HOST_INTERFACE
    if (power_save_on && wow.four_way_handshake) {
        ESP_LOGI(TAG, "Wakeup on FourWayHandshake");
        wake_host();
        buf_handle.flag = 0xFF;
        sleep(1);
    }
#endif

    /* Check destination address against self address */
    if (memcmp(ap_bssid, src_addr, MAC_ADDR_LEN)) {
        /* Check for multicast or broadcast address */
        //if (!(own_mac[0] & 1))
        ESP_LOG_BUFFER_HEXDUMP("src_addr", src_addr, MAC_ADDR_LEN, ESP_LOG_INFO);
        return ESP_FAIL;
    }

#if 0
    if (len) {
        ESP_LOG_BUFFER_HEXDUMP("RXEapol", buf, len, ESP_LOG_INFO);
    }
#endif

    tx_buf = (uint8_t *)malloc(len);
    if (!tx_buf) {
        ESP_LOGE(TAG, "%s:%u malloc failed\n", __func__, __LINE__);
        return ESP_FAIL;
    }

    memcpy((char *)tx_buf, buf, len);

    buf_handle.if_type = ESP_STA_IF;
    buf_handle.if_num = 0;
    buf_handle.payload_len = len;
    buf_handle.payload = tx_buf;
    buf_handle.wlan_buf_handle = tx_buf;
    buf_handle.free_buf_handle = free;
    buf_handle.pkt_type = PACKET_TYPE_EAPOL;

    ret = send_frame_to_host(&buf_handle);

    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send buffer\n");
        goto DONE;
    }

    return ESP_OK;

DONE:
    free(tx_buf);
    tx_buf = NULL;

    return ESP_FAIL;
}

static bool in_4way(void)
{
    ESP_LOGI(TAG, "STA in 4 way\n");
    return false;
}

static int sta_michael_mic_failure(u16 is_unicast)
{
    ESP_LOGI(TAG, "STA mic fail\n");
    return ESP_OK;
}

void disconnected_cb(uint8_t reason_code)
{
    ESP_LOGI(TAG, "STA disconnected [%u]\n", reason_code);
}

static int prepare_event(uint8_t if_type, interface_buffer_handle_t *buf_handle, uint16_t event_len)
{
    esp_err_t ret = ESP_OK;

    buf_handle->if_type = if_type;
    buf_handle->if_num = 0;
    buf_handle->payload_len = event_len;
    buf_handle->pkt_type = PACKET_TYPE_EVENT;

    buf_handle->payload = heap_caps_malloc(buf_handle->payload_len, MALLOC_CAP_DMA);
    if (!buf_handle->payload) {
        ESP_LOGE(TAG, "Failed to allocate event buffer\n");
        return ESP_FAIL;
    }
    memset(buf_handle->payload, 0, buf_handle->payload_len);

    buf_handle->priv_buffer_handle = buf_handle->payload;
    buf_handle->free_buf_handle = free;

    return ret;
}

void config_done(void)
{
}

void (wpa_sta_clear_current_pmksa)(void)
{
}

uint8_t *owe_build_dh_ie(uint16_t group)
{
    return NULL;
}

int process_owe_assoc_resp(const u8 *rsn_ie, size_t rsn_len, const uint8_t *dh_ie, size_t dh_len)
{
    return 0;
}

int wpa3_hostap_handle_auth(uint8_t *buf, size_t len, uint32_t type, uint16_t status, uint8_t *bssid)
{
    return 0;
}

static void handle_scan_event(void)
{
    interface_buffer_handle_t buf_handle = {0};
    struct event_header *header;
    esp_err_t ret = ESP_OK;

    ret = prepare_event(ESP_STA_IF, &buf_handle, sizeof(struct event_header));
    if (ret) {
        ESP_LOGE(TAG, "%s: Failed to prepare event buffer\n", __func__);
        return;
    }

    header = (struct event_header *) buf_handle.payload;

    header->event_code = EVENT_SCAN_RESULT;
    header->len = 0;
    header->status = 0;

    ret = send_command_event(&buf_handle);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send scan done event\n");
        goto DONE;
    }

    return;

DONE:
    if (buf_handle.payload) {
        free(buf_handle.payload);
        buf_handle.payload = NULL;
    }
}

int wpa2_sm_rx_eapol(uint8_t *src_addr, uint8_t *buf, uint32_t len, uint8_t *bssid)
{
    if (len >= 8) {
        uint8_t eapol_type = buf[1];
        uint8_t eap_code = buf[5];

        if (eapol_type == IEEE802_1X_TYPE_EAP_PACKET) {
            if (eap_code == EAP_CODE_SUCCESS) {
                ESP_LOGI(TAG, "EAP Success received");
                esp_wifi_set_wpa2_ent_state_internal(WPA2_ENT_EAP_STATE_SUCCESS);
            } else if (eap_code == EAP_CODE_FAILURE) {
                ESP_LOGI(TAG, "EAP Failure received");
                esp_wifi_set_wpa2_ent_state_internal(WPA2_ENT_EAP_STATE_FAIL);
            }
        }
    }

    return station_rx_eapol(src_addr, buf, len);
}

static int wpa2_start_eapol(void)
{
    return ESP_OK;
}

static int eap_peer_sm_init(void)
{
    esp_wifi_set_wpa2_ent_state_internal(WPA2_ENT_EAP_STATE_NOT_START);
    return 0;
}

static void eap_peer_sm_deinit(void)
{

}

static esp_err_t esp_client_enable_fn(void *arg)
{
    ESP_LOGI(TAG, "WiFi Enterprise enable for network_adapter");

    return ESP_OK;
}

static esp_err_t eap_client_disable_fn(void *param)
{
    cleanup_wpa2_cb();

    ESP_LOGI(TAG, "EAP disabled for network_adapter");
    return ESP_OK;
}

esp_err_t esp_hosted_sta_enterprise_enable(void)
{
    wifi_wpa2_param_t param;
    esp_err_t ret;

    param.fn = (wifi_wpa2_fn_t)esp_client_enable_fn;
    param.param = NULL;

    ret = esp_wifi_sta_wpa2_ent_enable_internal(&param);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable eap for network_adapter, ret=%d", ret);
    }

    return ret;
}

esp_err_t esp_hosted_sta_enterprise_disable(void)
{
    wifi_wpa2_param_t param;
    esp_err_t ret;


    param.fn = (wifi_wpa2_fn_t)eap_client_disable_fn;
    param.param = NULL;

    ret = esp_wifi_sta_wpa2_ent_disable_internal(&param);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable eap for network_adapter, ret=%d", ret);
    }

    return ret;
}
void handle_sta_disconnected_event(wifi_event_sta_disconnected_t *disconnected, bool wakeup_flag)
{
    interface_buffer_handle_t buf_handle = {0};
    struct disconnect_event *event;
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "STA Disconnect event: %d\n", disconnected->reason);

    cleanup_wpa2_cb();

    ret = prepare_event(ESP_STA_IF, &buf_handle, sizeof(struct disconnect_event));
    if (ret) {
        ESP_LOGE(TAG, "Failed to prepare event buffer, forcing restart\n");
        cleanup_ap_bssid();
        esp_restart();
        return;
    }

    event = (struct disconnect_event *) buf_handle.payload;

    event->header.event_code = EVENT_STA_DISCONNECT;
    event->header.len = htole16(buf_handle.payload_len - sizeof(struct event_header));
    event->header.status = s_disconnect_subtype;
    s_disconnect_subtype = DISCONNECT_TYPE_DEAUTH;

    memcpy(event->ssid, disconnected->ssid, disconnected->ssid_len);
    memcpy(event->bssid, disconnected->bssid, MAC_ADDR_LEN);
    event->reason = disconnected->reason;
    {
        uint16_t disc_seq = s_disconnect_generation ? s_disconnect_generation : s_conn_generation;
        event->disconnect_seq = htole16(disc_seq);
    }
    s_conn_generation = 0;
    s_disconnect_generation = 0;
    s_assoc_generation = 0;
    memset(s_assoc_bssid, 0, MAC_ADDR_LEN);
    s_assoc_control_port = 0;
    s_assoc_is_reassoc = 0;
    s_auth_generation = 0;
    station_authorized = 0;

    if (wakeup_flag) {
        buf_handle.flag = 0xFF;
    }
    ret = send_command_event(&buf_handle);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send disconnect event, forcing restart\n");
        esp_restart();
        goto DONE;
    }

    cleanup_ap_bssid();
    return;

DONE:
    if (buf_handle.payload) {
        free(buf_handle.payload);
        buf_handle.payload = NULL;
    }
    cleanup_ap_bssid();
    return;
}


static int sta_rx_assoc(uint8_t type, uint8_t *frame, size_t len, uint8_t *sender,
                        uint32_t rssi, uint8_t channel, uint64_t current_tsf)
{
    interface_buffer_handle_t buf_handle = {0};
    struct assoc_event *connect;
    esp_err_t ret = ESP_OK;
    struct ieee_mgmt_header *ieee_mh;
    uint8_t mac[MAC_ADDR_LEN];

    uint8_t expected_subtype = s_assoc_is_reassoc ? WLAN_FC_STYPE_REASSOC_RESP : WLAN_FC_STYPE_ASSOC_RESP;

    if (!s_assoc_generation || memcmp(s_assoc_bssid, sender, MAC_ADDR_LEN) != 0 || type != expected_subtype) {
        ESP_LOGI(TAG, "%s: Dropping unexpected or stale ASSOC frame type=%u from "MACSTR" (current_gen=%u expected_type=%u)\n",
                 __func__, type, MAC2STR(sender), s_assoc_generation, expected_subtype);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "STA connect event [channel %d]\n", channel);
    ESP_LOG_BUFFER_HEXDUMP("BSSID", sender, MAC_ADDR_LEN, ESP_LOG_INFO);

    ret = prepare_event(ESP_STA_IF, &buf_handle, sizeof(struct assoc_event)
                        + len + IEEE_HEADER_SIZE);
    if (ret) {
        ESP_LOGE(TAG, "%s: Failed to prepare event buffer, forcing restart\n", __func__);
        esp_restart();
        return ESP_FAIL;
    }

    connect = (struct assoc_event *) buf_handle.payload;

    connect->header.event_code = EVENT_ASSOC_RX;
    connect->header.len = htole16(buf_handle.payload_len - sizeof(struct event_header));
    connect->header.status = 0;

    /* Populate event body */
    connect->frame_type = type;
    connect->channel = channel;
    memcpy(connect->bssid, sender, MAC_ADDR_LEN);
    connect->assoc_seq = htole16(s_assoc_generation);
    bool assoc_success = (len >= 4 && frame[IE_POS_ASSOC_RESP_STATUS] == 0 &&
                          frame[IE_POS_ASSOC_RESP_STATUS + 1] == 0);

    if (assoc_success) {
        s_conn_generation = s_assoc_generation;
        if (!s_assoc_control_port) {
            station_authorized = 1;
        } else {
            station_authorized = 0;
        }
        /*
         * Keep association_ongoing set until WIFI_EVENT_STA_CONNECTED.
         * EAPOL 4-way handshake may start before that event.
         */
    } else {
        if (!station_connected) {
            /* Initial association failed: no established connection */
            s_conn_generation = 0;
            station_authorized = 0;
        }
        association_ongoing = 0;
    }

    s_assoc_generation = 0;
    memset(s_assoc_bssid, 0, MAC_ADDR_LEN);
    s_assoc_is_reassoc = 0;
    s_assoc_control_port = 0;
    connect->rssi = htole32(rssi);
    connect->tsf = htole64(current_tsf);

    /* Add IEEE mgmt header */
    ieee_mh = (struct ieee_mgmt_header *) connect->frame;

    ieee_mh->frame_control = type << 4;

    ret = esp_wifi_get_mac(WIFI_IF_STA, mac);
    memcpy(ieee_mh->da, mac, MAC_ADDR_LEN);
    memcpy(ieee_mh->sa, sender, MAC_ADDR_LEN);
    memcpy(ieee_mh->bssid, sender, MAC_ADDR_LEN);

    connect->frame_len = htole16(len + IEEE_HEADER_SIZE);
    memcpy(connect->frame + IEEE_HEADER_SIZE, frame, len);

    /*ESP_LOG_BUFFER_HEXDUMP(TAG, connect->frame, connect->frame_len, ESP_LOG_INFO);*/

    ret = send_command_event(&buf_handle);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send assoc resp event, forcing restart\n");
        esp_restart();
        goto DONE;
    }

    return ESP_OK;

DONE:
    if (buf_handle.payload) {
        free(buf_handle.payload);
        buf_handle.payload = NULL;
    }
    return ret;
}

static IRAM_ATTR void esp_wifi_tx_done_cb(uint8_t ifidx, uint8_t *data,
                                          uint16_t *len, bool txstatus)
{
    if (ifidx == WIFI_IF_STA) {
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    esp_err_t result;
    bool wakeup_flag = false;

    if (event_base != WIFI_EVENT) {
        ESP_LOGI(TAG, "Received unregistered event %s[%lu]\n", event_base, event_id);
        return;
    }

    switch (event_id) {

    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "station started");
        sta_init_flag = 1;
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Wifi Station Connected event!! \n");
        association_ongoing = 0;
        s_assoc_generation = 0;
        memset(s_assoc_bssid, 0, MAC_ADDR_LEN);
        s_assoc_control_port = 0;
        s_assoc_is_reassoc = 0;
        station_connected = 1;

        result = esp_wifi_set_tx_done_cb(esp_wifi_tx_done_cb);
        if (result) {
            ESP_LOGE(TAG, "Failed to set tx done cb\n");
        }

        result = esp_wifi_internal_reg_rxcb(WIFI_IF_STA, (wifi_rxcb_t)wlan_sta_rx_callback);
        if (result) {
            ESP_LOGE(TAG, "Failed to set rx cb\n");
        }

        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        if (!event_data) {
            ESP_LOGE(TAG, "%s:%u NULL data", __func__, __LINE__);
            return;
        }
        /* Clear association state on disconnect */
        association_ongoing = 0;
        s_assoc_generation = 0;
        memset(s_assoc_bssid, 0, MAC_ADDR_LEN);
        s_assoc_control_port = 0;
        s_assoc_is_reassoc = 0;
#if CONFIG_ESP_SDIO_HOST_INTERFACE
        if (power_save_on && wow.disconnect) {
            /* Wake-up host always on disconnect */
            ESP_LOGI(TAG, "Wakeup on disconnect");
            wake_host();
            wakeup_flag = true;
            sleep(1);
        }
#endif
        handle_sta_disconnected_event((wifi_event_sta_disconnected_t*) event_data, wakeup_flag);
        station_connected = 0;
        station_authorized = 0;
        /*esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);*/
        break;

    case WIFI_EVENT_SCAN_DONE:
        ESP_LOGI(TAG, "wifi scanning done");
        handle_scan_event();
        break;

    case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "softap started");
        softap_started = 1;
        break;

    case WIFI_EVENT_AP_STOP:
        ESP_LOGI(TAG, "softap stopped");
        softap_started = 0;
        break;

    case WIFI_EVENT_STA_STOP:
        ESP_LOGI(TAG, "Station stop");
        sta_init_flag = 0;
        association_ongoing = 0;
        s_assoc_generation = 0;
        memset(s_assoc_bssid, 0, MAC_ADDR_LEN);
        s_assoc_control_port = 0;
        s_assoc_is_reassoc = 0;
        station_authorized = 0;
        break;

    default:
        ESP_LOGI(TAG, "Unregistered event: %lu\n", event_id);
    }
}

void esp_create_wifi_event_loop(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
}

static int sta_rx_auth(uint8_t type, uint8_t *frame, size_t len, uint8_t *sender,
                       uint32_t rssi, uint8_t channel, uint64_t current_tsf)
{
    struct auth_event *event;
    esp_err_t ret = ESP_OK;
    interface_buffer_handle_t buf_handle = {0};
    struct ieee_mgmt_header *ieee_mh;
    uint8_t mac[MAC_ADDR_LEN];

    if (!s_auth_generation || memcmp(s_auth_bssid, sender, MAC_ADDR_LEN) != 0) {
        ESP_LOGI(TAG, "%s: Dropping unexpected or stale AUTH frame from "MACSTR" (current_gen=%u)\n",
                 __func__, MAC2STR(sender), s_auth_generation);
        return ESP_OK;
    }

    ret = prepare_event(ESP_STA_IF, &buf_handle, sizeof(struct auth_event)
                        + len + IEEE_HEADER_SIZE);
    if (ret) {
        ESP_LOGE(TAG, "%s: Failed to prepare event buffer, forcing restart\n", __func__);
        esp_restart();
        return ESP_FAIL;
    }

    event = (struct auth_event *) buf_handle.payload;

    /* Populate event header */
    event->header.event_code = EVENT_AUTH_RX;
    event->header.len = htole16(buf_handle.payload_len - sizeof(struct event_header));
    /*event->header.status = 1;*/

    /* Populate event body */
    event->frame_type = type;
    event->channel = channel;
    memcpy(event->bssid, sender, MAC_ADDR_LEN);
    event->auth_seq = htole16(s_auth_generation);
    s_auth_generation = 0;
    memset(s_auth_bssid, 0, MAC_ADDR_LEN);
    event->rssi = htole32(rssi);
    event->tsf = htole64(current_tsf);

    /* Add IEEE mgmt header */
    ieee_mh = (struct ieee_mgmt_header *) event->frame;

    ieee_mh->frame_control = type << 4;

    ret = esp_wifi_get_mac(WIFI_IF_STA, mac);
    memcpy(ieee_mh->da, mac, MAC_ADDR_LEN);
    memcpy(ieee_mh->sa, sender, MAC_ADDR_LEN);
    memcpy(ieee_mh->bssid, sender, MAC_ADDR_LEN);

    event->frame_len = htole16(len + IEEE_HEADER_SIZE);
    memcpy(event->frame + IEEE_HEADER_SIZE, frame, len);

    /*ESP_LOG_BUFFER_HEXDUMP(TAG, event->frame, event->frame_len, ESP_LOG_INFO);*/

    ret = send_command_event(&buf_handle);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send auth event, forcing restart\n");
        esp_restart();
        goto DONE;
    }

    return ESP_OK;

DONE:
    if (buf_handle.payload) {
        free(buf_handle.payload);
        buf_handle.payload = NULL;
    }
    return ret;
}

static int sta_rx_probe(uint8_t type, uint8_t *frame, size_t len, uint8_t *sender,
                        uint32_t rssi, uint8_t channel, uint64_t current_tsf)
{
    struct scan_event *event;
    esp_err_t ret = ESP_OK;
    interface_buffer_handle_t buf_handle = {0};

    /*ESP_LOGI(TAG, "SCAN# Type: %d, Channel: %d, Len: %lu\n", type, channel, len);
    ESP_LOG_BUFFER_HEXDUMP("Frame", frame, len, ESP_LOG_INFO);
    ESP_LOG_BUFFER_HEXDUMP("MAC", sender, MAC_ADDR_LEN, ESP_LOG_INFO);
    */

    ret = prepare_event(ESP_STA_IF, &buf_handle, sizeof(struct scan_event) + len);
    if (ret) {
        ESP_LOGE(TAG, "%s: Failed to prepare event buffer\n", __func__);
        return ESP_FAIL;
    }

    event = (struct scan_event *) buf_handle.payload;

    /* Populate event header */
    event->header.event_code = EVENT_SCAN_RESULT;
    event->header.len = htole16(buf_handle.payload_len - sizeof(struct event_header));
    event->header.status = 1;

    /* Populate event body */
    event->frame_type = type;
    event->channel = channel;
    memcpy(event->bssid, sender, MAC_ADDR_LEN);
    event->rssi = htole32(rssi);
    event->frame_len = htole16(len);
    event->tsf = htole64(current_tsf);
    memcpy(event->frame, frame, len);

    ret = send_command_event(&buf_handle);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send scan event\n");
        goto DONE;
    }

    return ESP_OK;

DONE:
    if (buf_handle.payload) {
        free(buf_handle.payload);
        buf_handle.payload = NULL;
    }
    return ret;
}

static int is_valid_assoc_resp(uint8_t *frame, size_t len, uint8_t *src_addr)
{
    /*ESP_LOG_BUFFER_HEXDUMP("assoc src addr", src_addr, MAC_ADDR_LEN, ESP_LOG_INFO);*/

    if (!len || !frame || !src_addr) {
        ESP_LOGE(TAG, "%s:%u Invalid args, Return failure\n", __func__, __LINE__);
        return false;
    }

    if (s_assoc_generation) {
        if (memcmp(s_assoc_bssid, src_addr, MAC_ADDR_LEN) != 0) {
            ESP_LOGI(TAG, "%s:%u Assoc response from unexpected AP, failure\n",
                     __func__, __LINE__);
            return false;
        }
    } else {
        if (!ap_bssid || memcmp(ap_bssid, src_addr, MAC_ADDR_LEN) != 0) {
            ESP_LOGI(TAG, "%s:%u Assoc response without active assoc or matching AP, failure\n",
                     __func__, __LINE__);
            return false;
        }
    }

    /*ESP_LOG_BUFFER_HEXDUMP("assoc frame:", frame, len, ESP_LOG_INFO);*/

    return true;
}

static int handle_wpa_sta_rx_mgmt(uint8_t type, uint8_t *frame, size_t len, uint8_t *sender,
                                  uint32_t rssi, uint8_t channel, uint64_t current_tsf)
{
    if (!sender) {
        ESP_LOGI(TAG, "%s:%u src mac addr NULL", __func__, __LINE__);
        return ESP_FAIL;
    }

    switch (type) {

    case WLAN_FC_STYPE_BEACON:
        /*ESP_LOGV(TAG, "%s:%u beacon frames ignored\n", __func__, __LINE__);*/
        sta_rx_probe(type, frame, len, sender, rssi, channel, current_tsf);
        break;

    case WLAN_FC_STYPE_PROBE_RESP:
        /*ESP_LOGV(TAG, "%s:%u probe response\n", __func__, __LINE__);*/
        sta_rx_probe(type, frame, len, sender, rssi, channel, current_tsf);
        break;

    case WLAN_FC_STYPE_AUTH:
        ESP_LOGI(TAG, "%s:%u Auth[%u] recvd\n", __func__, __LINE__, type);
        /*ESP_LOG_BUFFER_HEXDUMP(TAG, frame, len, ESP_LOG_INFO);*/
        sta_rx_auth(type, frame, len, sender, rssi, channel, current_tsf);
        break;

    case WLAN_FC_STYPE_ASSOC_RESP:
    case WLAN_FC_STYPE_REASSOC_RESP:

        ESP_LOGI(TAG, "%s:%u ASSOC Resp[%u] recvd\n", __func__, __LINE__, type);

        if (is_valid_assoc_resp(frame, len, sender)) {
            /* In case of open authentication,
             * connected event denotes connection established.
             * Whereas in case of secured authentication
             * It just triggers m1-m4 4 way handshake.
             */
            sta_rx_assoc(type, frame, len, sender, rssi, channel, current_tsf);
        }
        break;

    default:
        ESP_LOGI(TAG, "%s:%u Unsupported type[%u], ignoring\n", __func__, __LINE__, type);

    }

    return ESP_OK;
}

static int hostap_sta_join(uint8_t *bssid, uint8_t *wpa_ie, uint8_t wpa_ie_len,
                           uint8_t* rsnxe, uint16_t rsnxe_len, bool *pmf_enable, int subtype, uint8_t *pairwise_cipher)
{
    return true;
}

static int wpa_ap_remove(uint8_t* bssid)
{
    return true;
}

static uint8_t  *wpa_ap_get_wpa_ie(uint8_t *ie_len)
{
    *ie_len = 0;
    return NULL;
}

static bool wpa_ap_rx_eapol(uint8_t *addr, uint8_t *buf, size_t len)
{
    esp_err_t ret = ESP_OK;
    interface_buffer_handle_t buf_handle = {0};
    uint8_t * tx_buf = NULL;

    if (!buf || !len) {
        ESP_LOGI(TAG, "eapol err - buf: %p len: %zu\n",
                 buf, len);
        //TODO : free buf using esp_wifi_internal_free_rx_buffer?
        return ESP_FAIL;
    }

    tx_buf = (uint8_t *)malloc(len);
    if (!tx_buf) {
        ESP_LOGE(TAG, "%s:%u malloc failed\n", __func__, __LINE__);
        return ESP_FAIL;
    }

    memcpy((char *)tx_buf, buf, len);

#if 0
    if (len) {
        ESP_LOG_BUFFER_HEXDUMP("tx_buf", tx_buf, len, ESP_LOG_INFO);
    }
#endif

    buf_handle.if_type = ESP_AP_IF;
    buf_handle.if_num = 0;
    buf_handle.payload_len = len;
    buf_handle.payload = tx_buf;
    buf_handle.wlan_buf_handle = tx_buf;
    buf_handle.free_buf_handle = free;
    buf_handle.pkt_type = PACKET_TYPE_EAPOL;

    ESP_LOGD(TAG, "Sending eapol to host on AP iface\n");
    ret = send_frame_to_host(&buf_handle);

    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send buffer\n");
        goto DONE;
    }

    return true;

DONE:
    free(tx_buf);
    tx_buf = NULL;

    return false;
}

static void wpa_ap_get_peer_spp_msg(void *sm_data, bool *spp_cap, bool *spp_req)
{
    return;
}

char hostapd;
static int *hostap_init(void)
{
    return NULL;
}

static int hostap_deinit(void *data)
{
    return true;
}

static int handle_wpa_ap_rx_mgmt(void *pkt, uint32_t pkt_len, uint8_t chan, int rssi, int nf)
{
    struct mgmt_event *event;
    esp_err_t ret = ESP_OK;
    interface_buffer_handle_t buf_handle = {0};
    pkt_len = pkt_len + 24;

    //ESP_LOG_BUFFER_HEXDUMP(TAG, pkt, pkt_len, ESP_LOG_INFO);
    ret = prepare_event(ESP_AP_IF, &buf_handle, sizeof(struct mgmt_event)
                        + pkt_len);
    if (ret) {
        ESP_LOGE(TAG, "%s: Failed to prepare event buffer\n", __func__);
        return ESP_FAIL;
    }

    event = (struct mgmt_event *) buf_handle.payload;

    /* Populate event header */
    event->header.event_code = EVENT_AP_MGMT_RX;
    event->header.len = htole16(buf_handle.payload_len - sizeof(struct event_header));
    /*event->header.status = 1;*/

    memcpy(event->frame, pkt, pkt_len);
    event->frame_len = pkt_len;
    event->chan = chan;
    event->rssi = rssi;
    event->nf = nf;

#if 0
    if (event->frame[0] != 0x40) {
        ESP_LOGD(TAG, "%s: Got packet type as %x \n", __func__, event->frame[0]);
    }

    ESP_LOG_BUFFER_HEXDUMP(TAG, event->frame, event->frame_len, ESP_LOG_INFO);
#endif
    ret = send_command_event(&buf_handle);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send mgmt frames\n");
        goto DONE;
    }

    return ESP_OK;

DONE:
    if (buf_handle.payload) {
        free(buf_handle.payload);
        buf_handle.payload = NULL;
    }
    return ret;

    return 0;
}

static void sta_connected(uint8_t *bssid)
{
}

extern char * wpa_config_parse_string(const char *value, size_t *len);
static void sta_disconnected(uint8_t reason_code)
{
}

esp_err_t initialise_wifi(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    esp_err_t result = esp_wifi_init(&cfg);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Init internal failed");
        return result;
    }

    /* Disable Wi-Fi modem-sleep (default is WIFI_PS_MIN_MODEM). With modem
     * sleep enabled the AP buffers downlink frames and delivers them at the
     * DTIM beacon interval (~102ms), which shows up as a ~100ms sawtooth on
     * ping latency for traffic sourced from peers behind the AP. esp_hosted
     * is a line-powered adapter, so we trade power for minimum RX latency. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_wifi_set_debug_log();

    /* Register to get events from wifi driver */
    esp_create_wifi_event_loop();
    /* Register callback functions with wifi driver */
    memset(&wpa_cb, 0, sizeof(struct wpa_funcs));

    wpa_cb.wpa_sta_rx_mgmt = handle_wpa_sta_rx_mgmt;
    wpa_cb.wpa_sta_init = sta_init;
    wpa_cb.wpa_sta_deinit = sta_deinit;
    wpa_cb.wpa_sta_connect = sta_connection;
    wpa_cb.wpa_sta_connected_cb = sta_connected;
    wpa_cb.wpa_sta_disconnected_cb = sta_disconnected;
    wpa_cb.wpa_sta_disconnected_cb = disconnected_cb;
    wpa_cb.wpa_sta_rx_eapol = station_rx_eapol;
    wpa_cb.wpa_sta_in_4way_handshake = in_4way;
    wpa_cb.wpa_parse_wpa_ie = wpa_parse_wpa_ie_wrapper;
    wpa_cb.wpa_michael_mic_failure = sta_michael_mic_failure;
    wpa_cb.wpa_config_done = config_done;
    wpa_cb.wpa_config_parse_string  = wpa_config_parse_string;
    wpa_cb.wpa_sta_clear_curr_pmksa = wpa_sta_clear_current_pmksa;
    wpa_cb.owe_build_dhie = owe_build_dh_ie;
    wpa_cb.owe_process_assoc_resp = process_owe_assoc_resp;
    wpa_cb.wpa3_hostap_handle_auth = wpa3_hostap_handle_auth;

    wpa_cb.wpa_ap_join       = hostap_sta_join;
    wpa_cb.wpa_ap_remove     = wpa_ap_remove;
    wpa_cb.wpa_ap_get_wpa_ie = wpa_ap_get_wpa_ie;
    wpa_cb.wpa_ap_rx_eapol   = wpa_ap_rx_eapol;
    wpa_cb.wpa_ap_get_peer_spp_msg  = wpa_ap_get_peer_spp_msg;
    wpa_cb.wpa_ap_init       = hostap_init;
    wpa_cb.wpa_ap_deinit     = hostap_deinit;
    wpa_cb.wpa_ap_rx_mgmt    = handle_wpa_ap_rx_mgmt;

    esp_wifi_register_wpa_cb_internal(&wpa_cb);

    esp_hosted_sta_enterprise_enable();

    result = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (result) {
        ESP_LOGE(TAG, "Failed to set wifi mode\n");
    }

    return result;
}

int process_start_scan(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    uint32_t type = 0;
    wifi_scan_config_t params = {0};
    esp_err_t ret = ESP_OK;
    uint8_t cmd_status = CMD_RESPONSE_SUCCESS;
    struct scan_request *scan_req;
    bool config_present = false;

    if (!payload || payload_len < sizeof(*scan_req)) {
        ESP_LOGE(TAG, "SCAN_REJECT invalid_len=%u expected=%u", payload_len,
                 (unsigned)sizeof(*scan_req));
        return send_command_resp(if_type, CMD_SCAN_REQUEST, CMD_RESPONSE_INVALID, NULL, 0);
    }

    scan_req = (struct scan_request *) payload;

    if (strnlen(scan_req->ssid, sizeof(scan_req->ssid))) {
        params.ssid = malloc(sizeof(scan_req->ssid));
        if (!params.ssid) {
            ESP_LOGE(TAG, "Failed to allocate memory for scan SSID");
            return send_command_resp(if_type, CMD_SCAN_REQUEST, CMD_RESPONSE_FAIL, NULL, 0);
        }

        memcpy(params.ssid, scan_req->ssid, sizeof(scan_req->ssid));
        params.scan_type = 0;
        config_present = true;
    }

    if (scan_req->channel) {
        params.channel = scan_req->channel;
        config_present = true;
    }

    if (memcmp(scan_req->bssid, zero_mac, MAC_ADDR_LEN) != 0 &&
        !IS_BROADCAST_ADDR(scan_req->bssid)) {
        params.bssid = malloc(sizeof(scan_req->bssid));
        if (!params.bssid) {
            ESP_LOGE(TAG, "Failed to allocate memory for scan BSSID");
            if (params.ssid) {
                free(params.ssid);
                params.ssid = NULL;
            }
            return send_command_resp(if_type, CMD_SCAN_REQUEST, CMD_RESPONSE_FAIL, NULL, 0);
        }
        memcpy(params.bssid, scan_req->bssid, sizeof(scan_req->bssid));
        config_present = true;
    }

    if (sta_init_flag || softap_started) {
        /* Register to receive probe response and beacon frames */
        type = (1 << WLAN_FC_STYPE_BEACON) | (1 << WLAN_FC_STYPE_PROBE_RESP) |
               (1 << WLAN_FC_STYPE_ASSOC_RESP) | (1 << WLAN_FC_STYPE_REASSOC_RESP);
        esp_wifi_register_mgmt_frame_internal(type, 0);

        /* Trigger scan */
        if (config_present) {
            ret = esp_wifi_scan_start(&params, false);
        } else {
            ret = esp_wifi_scan_start(NULL, false);
        }

        if (ret) {
            ESP_LOGI(TAG, "Scan failed ret=[0x%x]\n", ret);
            cmd_status = CMD_RESPONSE_FAIL;

            /* Reset frame registration */
            esp_wifi_register_mgmt_frame_internal(0, 0);
        }
    } else {
        ESP_LOGI(TAG, "Scan not permitted as WiFi is not yet up");
        cmd_status = CMD_RESPONSE_FAIL;
    }

    if (params.ssid) {
        free(params.ssid);
        params.ssid = NULL;
    }
    if (params.bssid) {
        free(params.bssid);
        params.bssid = NULL;
    }

    ret = send_command_resp(if_type, CMD_SCAN_REQUEST, cmd_status, NULL, 0);

    return ret;
}

int process_set_mcast_mac_list(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    struct cmd_set_mcast_mac_addr *cmd_mcast_mac_list;

    cmd_mcast_mac_list = (struct cmd_set_mcast_mac_addr *) payload;

    if (cmd_mcast_mac_list->count > MAX_MULTICAST_ADDR_COUNT) {
        ESP_LOGE(TAG, "CMD_RX_INVALID: mcast count=%u exceeds max %u",
                 cmd_mcast_mac_list->count, MAX_MULTICAST_ADDR_COUNT);
        return send_command_resp(if_type, CMD_SET_MCAST_MAC_ADDR, CMD_RESPONSE_FAIL, NULL, 0);
    }

    mac_list.count = cmd_mcast_mac_list->count;
    memcpy(mac_list.mac_addr, cmd_mcast_mac_list->mcast_addr,
           sizeof(mac_list.mac_addr));

    /*ESP_LOG_BUFFER_HEXDUMP("MAC Filter", (uint8_t *) &mac_list, sizeof(mac_list), ESP_LOG_INFO);*/

    ret = send_command_resp(if_type, CMD_SET_MCAST_MAC_ADDR, CMD_RESPONSE_SUCCESS, NULL, 0);

    return ret;
}

int process_tx_power(uint8_t if_type, uint8_t *payload, uint16_t payload_len, uint8_t cmd)
{
    esp_err_t ret = ESP_OK;
    struct cmd_set_get_val *val;
    int8_t max_tx_power;
    uint32_t value;

    if (cmd == CMD_SET_TXPOWER) {
        val = (struct cmd_set_get_val *)payload;
        max_tx_power = val->value;
        esp_wifi_set_max_tx_power(max_tx_power);
    }

    esp_wifi_get_max_tx_power((int8_t *)&max_tx_power);
    value = max_tx_power;
    ret = send_command_resp(if_type, cmd, CMD_RESPONSE_SUCCESS, (uint8_t *)&value,
                            sizeof(uint32_t));

    return ret;
}

int process_get_tx_power(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    int8_t max_tx_power = 0;
    uint32_t wire_value;
    esp_err_t ret = esp_wifi_get_max_tx_power(&max_tx_power);

    if (ret != ESP_OK)
        return send_command_resp(if_type, CMD_GET_TXPOWER, CMD_RESPONSE_FAIL, NULL, 0);
    wire_value = htole32((uint32_t)(uint8_t)max_tx_power);
    return send_command_resp(if_type, CMD_GET_TXPOWER,
                             CMD_RESPONSE_SUCCESS,
                             (uint8_t *)&wire_value,
                             sizeof(wire_value));
}

int process_set_tx_power(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    struct cmd_set_get_val *val;
    int8_t max_tx_power;
    uint32_t wire_value;
    esp_err_t ret;

    if (!payload || payload_len < sizeof(*val))
        return send_command_resp(if_type, CMD_SET_TXPOWER, CMD_RESPONSE_INVALID, NULL, 0);
    val = (struct cmd_set_get_val *)payload;
    max_tx_power = (int8_t)le32toh(val->value);
    ret = esp_wifi_set_max_tx_power(max_tx_power);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SET_TXPOWER failed ret=%d", ret);
        return send_command_resp(if_type, CMD_SET_TXPOWER, CMD_RESPONSE_FAIL, NULL, 0);
    }
    ret = esp_wifi_get_max_tx_power(&max_tx_power);
    if (ret != ESP_OK)
        return send_command_resp(if_type, CMD_SET_TXPOWER, CMD_RESPONSE_FAIL, NULL, 0);

    wire_value = htole32((uint32_t)(uint8_t)max_tx_power);
    return send_command_resp(if_type, CMD_SET_TXPOWER,
                             CMD_RESPONSE_SUCCESS,
                             (uint8_t *)&wire_value,
                             sizeof(wire_value));
}

int process_rssi(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    wifi_ap_record_t ap_info = {0};

    ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret != ESP_OK) {
        return send_command_resp(if_type, CMD_STA_RSSI, CMD_RESPONSE_FAIL, NULL, 0);
    }

    return send_command_resp(if_type, CMD_STA_RSSI, CMD_RESPONSE_SUCCESS, (uint8_t *) &ap_info.rssi, sizeof(int8_t));
}

int process_set_ip(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    struct cmd_set_ip_addr *cmd_set_ip;

    cmd_set_ip = (struct cmd_set_ip_addr *) payload;

    ip_address = le32toh(cmd_set_ip->ip);

    ret = send_command_resp(if_type, CMD_SET_IP_ADDR, CMD_RESPONSE_SUCCESS, NULL, 0);

    return ret;
}

int process_wow_set(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    struct cmd_wow_config *cmd;

    cmd = (struct cmd_wow_config *)payload;

    wow.any = cmd->any;
    wow.disconnect = cmd->disconnect;
    wow.magic_pkt = cmd->magic_pkt;
    wow.four_way_handshake = cmd->four_way_handshake;
    wow.eap_identity_req = cmd->eap_identity_req;

    if (cmd->any) {
        wow.disconnect = 1;
        wow.magic_pkt = 1;
        wow.four_way_handshake = 1;
        wow.eap_identity_req = 1;
    }

    return send_command_resp(if_type, CMD_SET_WOW_CONFIG, CMD_RESPONSE_SUCCESS, NULL, 0);
}

int process_set_time(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    struct cmd_set_time *cmd;
    struct timeval tv;
    int ret;

    if (!payload || payload_len < sizeof(*cmd))
        return send_command_resp(if_type, CMD_SET_TIME, CMD_RESPONSE_INVALID, NULL, 0);
    cmd = (struct cmd_set_time *)payload;
    tv.tv_sec = (time_t)le64toh(cmd->sec);
    tv.tv_usec = (suseconds_t)le64toh(cmd->usec);
    ret = settimeofday(&tv, NULL);
    if (ret) {
        ESP_LOGE(TAG, "SET_TIME failed ret=%d", ret);
        return send_command_resp(if_type, CMD_SET_TIME, CMD_RESPONSE_FAIL, NULL, 0);
    }
    return send_command_resp(if_type, CMD_SET_TIME, CMD_RESPONSE_SUCCESS, NULL, 0);
}

int process_reg_set(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    struct cmd_reg_domain *cmd;
    esp_err_t ret;
    uint8_t status;

    if (!payload || payload_len < sizeof(*cmd))
        return send_command_resp(if_type, CMD_SET_REG_DOMAIN, CMD_RESPONSE_INVALID, NULL, 0);
    cmd = (struct cmd_reg_domain *)payload;
    ret = esp_wifi_set_country_code(cmd->country_code, false);
    status = ret == ESP_OK ? CMD_RESPONSE_SUCCESS : CMD_RESPONSE_FAIL;
    if (ret != ESP_OK)
        ESP_LOGE(TAG, "SET_REG %.2s failed ret=%d", cmd->country_code, ret);
    return send_command_resp(if_type, CMD_SET_REG_DOMAIN, status,
                             (uint8_t *)cmd->country_code,
                             sizeof(cmd->country_code));
}

int process_ota_start(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    uint16_t cmd_status = CMD_RESPONSE_SUCCESS;
    esp_err_t ret = ESP_OK;

    if (handle) {
        esp_ota_abort(handle);
        handle = 0;
    }
    verify_ota = false;
    update_partition = NULL;

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get next update partition");
        cmd_status = CMD_RESPONSE_FAIL;
        goto send_resp;
    }

    ret = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (ret) {
        ESP_LOGE(TAG, "OTA update failed in OTA begin");
        cmd_status = CMD_RESPONSE_FAIL;
        handle = 0;
        update_partition = NULL;
        goto send_resp;
    }

    ESP_LOGI(TAG, "ESP OTA begin start");

send_resp:
    ret = send_command_resp(if_type, CMD_START_OTA_UPDATE, cmd_status, NULL, 0);
    return ret;

}

int verify_ota_image_header(char *binary_image)
{
    esp_image_header_t *img_header = (esp_image_header_t *)binary_image;

    //verify image CHIP ID
    if (img_header->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        ESP_LOGE(TAG, "Firmware provided for ota has different chip id %x expected chip id %x", img_header->chip_id, CONFIG_IDF_FIRMWARE_CHIP_ID);
        return -1;
    }

    verify_ota = true;
    return 0;
}

int process_ota_write(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    struct cmd_ota_update_request *cmd;
    uint8_t cmd_status = CMD_RESPONSE_SUCCESS;

    if (!payload ||
        payload_len < offsetof(struct cmd_ota_update_request, ota_binary)) {
        ESP_LOGE(TAG, "OTA write invalid fixed length=%u", payload_len);
        cmd_status = CMD_RESPONSE_INVALID;
        goto send_resp;
    }

    cmd = (struct cmd_ota_update_request *)(payload);
    uint16_t ota_binary_len = le16toh(cmd->ota_binary_len);
    if (!ota_binary_len ||
        !cmd_flex_len_valid(payload_len,
                            offsetof(struct cmd_ota_update_request, ota_binary),
                            ota_binary_len) ||
        (!verify_ota && ota_binary_len < sizeof(esp_image_header_t))) {
        ESP_LOGE(TAG, "OTA write invalid payload_len=%u binary_len=%u verify=%u",
                 payload_len, ota_binary_len, verify_ota);
        cmd_status = CMD_RESPONSE_INVALID;
        goto send_resp;
    }

    if (!verify_ota) {
        ret = verify_ota_image_header(cmd->ota_binary);
        if (ret != 0) {
            goto fail;
        }
    }

    ret = esp_ota_write(handle, (const void *)cmd->ota_binary,
                        ota_binary_len);

fail:
    if (ret) {
        if (handle) {
            esp_ota_abort(handle);
            handle = 0;
        }
        update_partition = NULL;
        verify_ota = false;
        ESP_LOGE(TAG, "OTA update failed in OTA Write");
        cmd_status = CMD_RESPONSE_FAIL;
    }

send_resp:
    ret = send_command_resp(if_type, CMD_START_OTA_WRITE, cmd_status, NULL, 0);
    return ret;
}

static void esp_reset_callback(TimerHandle_t xTimer)
{
    xTimerDelete(xTimer, 0);
    esp_restart();
}

int process_ota_end(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    uint8_t cmd_status = CMD_RESPONSE_SUCCESS;
    TimerHandle_t xTimer = NULL;
    const esp_partition_t *old_boot_partition = NULL;

    ret = esp_ota_end(handle);
    handle = 0;
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "OTA update failed in end (%s)!", esp_err_to_name(ret));
        }
        cmd_status = CMD_RESPONSE_FAIL;
        goto fail;
    }

    xTimer = xTimerCreate("Reset Timer", RESET_TIMEOUT, pdFALSE, 0, esp_reset_callback);
    if (xTimer == NULL) {
        ESP_LOGE(TAG, "Failed to create timer to restart system");
        cmd_status = CMD_RESPONSE_FAIL;
        goto fail;
    }

    old_boot_partition = esp_ota_get_boot_partition();

    /* set OTA partition for next boot */
    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(ret));
        xTimerDelete(xTimer, 0);
        xTimer = NULL;
        cmd_status = CMD_RESPONSE_FAIL;
        goto fail;
    }

    ret = xTimerStart(xTimer, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to start timer to restart system, attempting partition rollback");
        if (old_boot_partition && esp_ota_set_boot_partition(old_boot_partition) != ESP_OK) {
            ESP_LOGE(TAG, "Partition rollback failed, forcing restart");
            esp_restart();
        }
        xTimerDelete(xTimer, 0);
        xTimer = NULL;
        cmd_status = CMD_RESPONSE_FAIL;
        goto fail;
    }

    ESP_LOGE(TAG, "**** OTA updated successful, ESP will reboot in 5 sec ****");
fail:
    if (cmd_status != CMD_RESPONSE_SUCCESS && handle) {
        esp_ota_abort(handle);
    }
    handle = 0;
    update_partition = NULL;
    verify_ota = false;
    ESP_LOGI(TAG, "ESP OTA end");

    ret = send_command_resp(if_type, CMD_START_OTA_END, cmd_status, NULL, 0);

    return ret;
}

int process_reg_get(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    char country_code[4] = {0};
    esp_err_t ret = esp_wifi_get_country_code(country_code);

    if (ret != ESP_OK)
        return send_command_resp(if_type, CMD_GET_REG_DOMAIN, CMD_RESPONSE_FAIL, NULL, 0);
    return send_command_resp(if_type, CMD_GET_REG_DOMAIN,
                             CMD_RESPONSE_SUCCESS,
                             (uint8_t *)country_code,
                             sizeof(country_code));
}

int ieee80211_delete_node(uint8_t *mac);
int process_disconnect(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    struct cmd_disconnect *cmd;
    wifi_mode_t wifi_mode = WIFI_MODE_NULL;
    uint16_t reason;
    uint8_t status = CMD_RESPONSE_FAIL;
    bool had_sta_session;
    esp_err_t ret;

    if (!payload || payload_len < sizeof(*cmd))
        return send_command_resp(if_type, CMD_DISCONNECT, CMD_RESPONSE_INVALID, NULL, 0);

    cmd = (struct cmd_disconnect *)payload;
    reason = le16toh(cmd->reason_code);
    had_sta_session = station_connected || association_ongoing;

    ret = esp_wifi_get_mode(&wifi_mode);
    if (ret != ESP_OK)
        goto resp;

    if (if_type == ESP_STA_IF) {
        if (cmd->header.reserved1 == DISCONNECT_TYPE_LOCAL) {
            /* Firmware Wi-Fi stack has no local-only disconnect primitive without
             * over-the-air deauthentication. Return unsupported rather than performing
             * an over-the-air disconnect. */
            status = CMD_RESPONSE_UNSUPPORTED;
        } else if (cmd->header.reserved1 == DISCONNECT_TYPE_DISASSOC) {
            /* Firmware Wi-Fi stack has no independent disassociation primitive without
             * deauthentication. Return unsupported rather than performing deauthentication. */
            status = CMD_RESPONSE_UNSUPPORTED;
        } else if (sta_init_flag && had_sta_session &&
            (wifi_mode == WIFI_MODE_STA || wifi_mode == WIFI_MODE_APSTA)) {
            s_disconnect_generation = active_cmd_seq;
            s_disconnect_subtype = cmd->header.reserved1;
            s_assoc_generation = 0;
            memset(s_assoc_bssid, 0, MAC_ADDR_LEN);
            s_assoc_control_port = 0;
            s_assoc_is_reassoc = 0;
            association_ongoing = 0;
            esp_wifi_deauthenticate_internal(reason);
            status = CMD_RESPONSE_SUCCESS;
        }
    } else if (if_type == ESP_AP_IF) {
        if (softap_started &&
            (wifi_mode == WIFI_MODE_AP || wifi_mode == WIFI_MODE_APSTA)) {
            int del_ret = ieee80211_delete_node(cmd->mac);
            if (del_ret == 0)
                status = CMD_RESPONSE_SUCCESS;
            else
                status = CMD_RESPONSE_FAIL;
        }
    } else {
        status = CMD_RESPONSE_INVALID;
    }

resp:
    return send_command_resp(if_type, CMD_DISCONNECT, status, NULL, 0);
}

int process_auth_request(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    struct cmd_sta_auth *cmd_auth;
    wifi_config_t wifi_config = {0};
    uint32_t type = 0;
    uint16_t number = DEFAULT_SCAN_LIST_SIZE;
    wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE] = {0};
    uint8_t auth_type = 0;
    uint8_t msg_type = 0, *pos;
    uint8_t found_ssid = 0;
    uint8_t cmd_status = CMD_RESPONSE_SUCCESS;
    bool enterprise_cb_created = false;

    if (!payload || payload_len < offsetof(struct cmd_sta_auth, auth_data)) {
        ESP_LOGE(TAG, "AUTH_REJECT invalid fixed length=%u", payload_len);
        return send_command_resp(if_type, CMD_STA_AUTH, CMD_RESPONSE_INVALID, NULL, 0);
    }

    cmd_auth = (struct cmd_sta_auth *) payload;
    if (cmd_auth->key_len > sizeof(cmd_auth->key) ||
        !cmd_flex_len_valid(payload_len,
                            offsetof(struct cmd_sta_auth, auth_data),
                            cmd_auth->auth_data_len) ||
        (cmd_auth->auth_data_len && cmd_auth->auth_data_len < 4)) {
        ESP_LOGE(TAG, "AUTH_REJECT invalid nested length payload=%u key=%u auth=%u",
                 payload_len, cmd_auth->key_len, cmd_auth->auth_data_len);
        return send_command_resp(if_type, CMD_STA_AUTH, CMD_RESPONSE_INVALID, NULL, 0);
    }

#define WLAN_AUTH_OPEN 0
#define WLAN_AUTH_FT 2
#define WLAN_AUTH_SAE 3

    /* Auth data generally present in WPA3 frames. Byte 0 is the 802.11
     * Authentication transaction sequence: 1 = SAE commit, 2 = SAE confirm. */
    if (cmd_auth->auth_data_len) {
        pos = (uint8_t *) cmd_auth->auth_data;
        msg_type = *pos;
    }

    /* A new connection attempt while another is in flight would overlap two
     * attempts. SAE confirm (seq 2) and a same-BSSID SAE commit retry (seq 1,
     * anti-clogging token) are continuations of the in-flight attempt. */
    if (association_ongoing && msg_type != 2) {
        wifi_config_t cur_cfg = {0};
        bool sae_commit_retry = (msg_type == 1 &&
                                 cmd_auth->auth_type == WLAN_AUTH_SAE &&
                                 esp_wifi_get_config(WIFI_IF_STA, &cur_cfg) == ESP_OK &&
                                 memcmp(cur_cfg.sta.bssid, cmd_auth->bssid,
                                        MAC_ADDR_LEN) == 0);

        if (!sae_commit_retry) {
            ESP_LOGW(TAG, "AUTH_REJECT busy connecting=1 connected=%u seq=%u msg=%u",
                     station_connected, active_cmd_seq, msg_type);
            return send_command_resp(if_type, CMD_STA_AUTH, CMD_RESPONSE_BUSY, NULL, 0);
        }
    }

    s_auth_generation = active_cmd_seq;
    memcpy(s_auth_bssid, cmd_auth->bssid, MAC_ADDR_LEN);
    memcpy(s_assoc_bssid, cmd_auth->bssid, MAC_ADDR_LEN);
    esp_wifi_unset_appie_internal(WIFI_APPIE_RAM_STA_AUTH);
    if (cmd_auth->auth_data_len) {
        esp_wifi_set_appie_internal(WIFI_APPIE_RAM_STA_AUTH, cmd_auth->auth_data + 4,
                                    cmd_auth->auth_data_len - 4, 0);
    }

    if (msg_type == 2) {
        /* WPA3 specific */
        ESP_LOGI(TAG, "AUTH Confirm\n");
        ret = esp_wifi_send_auth_internal(0, cmd_auth->bssid, cmd_auth->auth_type, 2, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "AUTH Confirm submission failed ret=%d", ret);
            s_auth_generation = 0;
            esp_wifi_unset_appie_internal(WIFI_APPIE_RAM_STA_AUTH);
            cmd_status = CMD_RESPONSE_FAIL;
            goto send_resp;
        }

    } else if (association_ongoing && msg_type == 1) {
        /* Same-BSSID SAE commit retry (anti-clogging token). IEs already
         * refreshed; do not restart esp_wifi_connect(). */
        ESP_LOGI(TAG, "AUTH SAE commit retry");
        ret = esp_wifi_send_auth_internal(0, cmd_auth->bssid, cmd_auth->auth_type, 1, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "AUTH SAE commit retry submission failed ret=%d", ret);
            s_auth_generation = 0;
            esp_wifi_unset_appie_internal(WIFI_APPIE_RAM_STA_AUTH);
            cmd_status = CMD_RESPONSE_FAIL;
            goto send_resp;
        }

    } else {
        wifi_scan_config_t params = {0};

        params.bssid = malloc(sizeof(cmd_auth->bssid));
        if (!params.bssid) {
            ESP_LOGE(TAG, "AUTH_REJECT BSSID allocation failed");
            cmd_status = CMD_RESPONSE_FAIL;
            goto send_resp;
        }

        memcpy(params.bssid, cmd_auth->bssid, sizeof(cmd_auth->bssid));
        params.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        params.show_hidden = true;

        ESP_LOGI(TAG, "channel is %d\n", cmd_auth->channel);
        if (cmd_auth->channel) {
            params.channel = cmd_auth->channel;
        }

        esp_wifi_scan_start(&params, true);

        free(params.bssid);
        ret = esp_wifi_scan_get_ap_records(&number, ap_info);
        if (ret) {
            ESP_LOGI(TAG, "Err: esp_wifi_scan_get_ap_records: %d\n", ret);
        }

        /* Register the Management frames */
        type = (1 << WLAN_FC_STYPE_ASSOC_RESP)
               | (1 << WLAN_FC_STYPE_REASSOC_RESP)
               | (1 << WLAN_FC_STYPE_AUTH)
               | (1 << WLAN_FC_STYPE_DEAUTH)
               | (1 << WLAN_FC_STYPE_DISASSOC);

        esp_wifi_register_mgmt_frame_internal(type, 0);

        /* ESP_LOG_BUFFER_HEXDUMP("BSSID", cmd_auth->bssid, MAC_ADDR_LEN, ESP_LOG_INFO); */
        for (int i = 0; !found_ssid &&
             (i < DEFAULT_SCAN_LIST_SIZE) && (i < number); i++) {
            /*ESP_LOG_BUFFER_HEXDUMP("Next BSSID", ap_info[i].bssid, MAC_ADDR_LEN, ESP_LOG_INFO);
              ESP_LOGI(TAG, "ssid: %s, authmode: %u", ap_info[i].ssid, ap_info[i].authmode);*/
            if (memcmp(ap_info[i].bssid, cmd_auth->bssid, MAC_ADDR_LEN) == 0) {
                auth_type = ap_info[i].authmode;
                found_ssid = 1;
                break;
            }
        }

        if (!found_ssid) {
            ESP_LOGI(TAG, "AP not found to connect.");
            cmd_status = CMD_RESPONSE_FAIL;
            goto send_resp;
        }

        memcpy(wifi_config.sta.ssid, cmd_auth->ssid, MAX_SSID_LEN);
        /* ESP_LOGI(TAG, "ssid_found:%u Auth type scanned[%u], exp[%u] for ssid %s", found_ssid, auth_type, cmd_auth->auth_type, wifi_config.sta.ssid); */

        ESP_LOGI(TAG, "Connecting to %.*s, channel: %u [%d]", MAX_SSID_LEN,
                 wifi_config.sta.ssid, cmd_auth->channel, auth_type);

        if (auth_type == WIFI_AUTH_WEP) {
            if (!cmd_auth->key_len) {
                ESP_LOGE(TAG, "WEP password not present");
            }
            memcpy(wifi_config.sta.password, cmd_auth->key, 27);
            wifi_config.sta.threshold.authmode = WIFI_AUTH_WEP;
        } else if (auth_type == WIFI_AUTH_OWE) {
            wifi_config.sta.owe_enabled = 1;
        } else if (auth_type != WIFI_AUTH_OPEN) {
            memcpy(wifi_config.sta.password, DUMMY_PASSPHRASE, sizeof(DUMMY_PASSPHRASE));
            wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;;
        }

	if (auth_type == WIFI_AUTH_WPA2_ENTERPRISE ||
			auth_type == WIFI_AUTH_WPA3_ENT_192 ||
			auth_type == WIFI_AUTH_WPA3_ENTERPRISE ||
			auth_type == WIFI_AUTH_WPA2_WPA3_ENTERPRISE ||
			auth_type == WIFI_AUTH_WPA_ENTERPRISE) {
		cleanup_wpa2_cb();
		wpa2_cb = (struct wpa2_funcs*)malloc(sizeof(struct wpa2_funcs));
		if (!wpa2_cb) {
			ESP_LOGE(TAG, "Enterprise WPA callback allocation failed");
			cmd_status = CMD_RESPONSE_FAIL;
			goto send_resp;
		}
		wpa2_cb->wpa2_sm_rx_eapol = wpa2_sm_rx_eapol;
		wpa2_cb->wpa2_start = wpa2_start_eapol;
		wpa2_cb->wpa2_init = eap_peer_sm_init;
		wpa2_cb->wpa2_deinit = eap_peer_sm_deinit;
		ret = esp_wifi_register_wpa2_cb_internal(wpa2_cb);
		if (ret) {
			ESP_LOGE(TAG, "Enterprise WPA callback registration failed ret=%d", ret);
			free(wpa2_cb);
			wpa2_cb = NULL;
			cmd_status = CMD_RESPONSE_FAIL;
			goto send_resp;
		}
		enterprise_cb_created = true;
	}

        ESP_LOGD(TAG, "AUTH type=%d password used=%s\n", auth_type, wifi_config.sta.password);
        memcpy(wifi_config.sta.bssid, cmd_auth->bssid, MAC_ADDR_LEN);
        wifi_config.sta.bssid_set = 1;

        wifi_config.sta.channel = cmd_auth->channel;

        /* Common handling for rest sec prot */
        ESP_LOGI(TAG, "AUTH Commit");

#if 0
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
        wifi_protocols_t protocols = {
            .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
            .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N,
        };
        ret = esp_wifi_set_protocols(WIFI_IF_STA, &protocols);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set protocols ret=%d", ret);
        }
        wifi_bandwidths_t bw = {
            .ghz_2g = WIFI_BW_HT40,
            .ghz_5g = WIFI_BW_HT40,
        };
        ret = esp_wifi_set_bandwidths(WIFI_IF_STA, &bw);
        if (ret) {
            ESP_LOGE(TAG, "Failed to set wifi bandwidth: %d\n", ret);
        }
#else
        ret = esp_wifi_set_protocol(WIFI_IF_STA,
                WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set protocol ret=%d", ret);
        }
        ret = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
        if (ret) {
            ESP_LOGE(TAG, "Failed to set wifi bandwidth: %d\n", ret);
        }
#endif
#endif
        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (ret) {
            ESP_LOGE(TAG, "Failed to set wifi config: %d\n", ret);
            cmd_status = CMD_RESPONSE_FAIL;
            goto send_resp;
        }

        /* This API sends auth commit to AP */
        ret = esp_wifi_connect();
        if (ret) {
            ESP_LOGE(TAG, "Failed to connect wifi\n");
            cmd_status = CMD_RESPONSE_FAIL;
            goto send_resp;
        }
    }
    association_ongoing = 1;

send_resp:
    if (cmd_status != CMD_RESPONSE_SUCCESS) {
        s_auth_generation = 0;
        memset(s_auth_bssid, 0, MAC_ADDR_LEN);
        if (enterprise_cb_created)
            cleanup_wpa2_cb();
    }
    ret = send_command_resp(if_type, CMD_STA_AUTH, cmd_status, NULL, 0);

    return ret;
}

int process_assoc_request(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    struct cmd_sta_assoc *cmd_assoc;
    uint16_t cmd_status = CMD_RESPONSE_SUCCESS;

    if (!payload || payload_len < offsetof(struct cmd_sta_assoc, assoc_ie)) {
        ESP_LOGE(TAG, "ASSOC_REJECT invalid fixed length=%u", payload_len);
        return send_command_resp(if_type, CMD_STA_ASSOC, CMD_RESPONSE_INVALID, NULL, 0);
    }
    cmd_assoc = (struct cmd_sta_assoc *) payload;
    if (!cmd_flex_len_valid(payload_len,
                            offsetof(struct cmd_sta_assoc, assoc_ie),
                            cmd_assoc->assoc_ie_len)) {
        ESP_LOGE(TAG, "ASSOC_REJECT invalid nested length payload=%u ie=%u",
                 payload_len, cmd_assoc->assoc_ie_len);
        return send_command_resp(if_type, CMD_STA_ASSOC, CMD_RESPONSE_INVALID, NULL, 0);
    }

    if (s_assoc_generation != 0) {
        ESP_LOGW(TAG, "ASSOC_REJECT busy s_assoc_generation=%u seq=%u", s_assoc_generation, active_cmd_seq);
        return send_command_resp(if_type, CMD_STA_ASSOC, CMD_RESPONSE_BUSY, NULL, 0);
    }

    esp_wifi_unset_appie_internal(WIFI_APPIE_ASSOC_REQ);
    if (cmd_assoc->assoc_ie_len) {
        ret = esp_wifi_set_appie_internal(WIFI_APPIE_ASSOC_REQ, cmd_assoc->assoc_ie,
                                          cmd_assoc->assoc_ie_len, 0);
        if (ret) {
            ESP_LOGE(TAG, "Failed to set assoc IE ret=%d", ret);
            cmd_status = CMD_RESPONSE_FAIL;
            goto send_resp;
        }
    }
#define WLAN_FC_STYPE_ASSOC_REQ         0
#define WLAN_FC_STYPE_REASSOC_REQ       2
#define WLAN_STATUS_SUCCESS 0
    association_ongoing = 1;
    s_assoc_generation = active_cmd_seq;
    {
        wifi_config_t cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.bssid_set) {
            memcpy(s_assoc_bssid, cfg.sta.bssid, MAC_ADDR_LEN);
        }
    }
    s_assoc_control_port = cmd_assoc->control_port ? 1 : 0;
    s_assoc_is_reassoc = cmd_assoc->is_reassoc ? 1 : 0;
    {
        uint8_t sub_type = cmd_assoc->is_reassoc ? WLAN_FC_STYPE_REASSOC_REQ : WLAN_FC_STYPE_ASSOC_REQ;
        ret = esp_wifi_send_assoc_internal(WIFI_IF_STA, NULL, sub_type, WLAN_STATUS_SUCCESS);
    }
    if (ret) {
        ESP_LOGE(TAG, "Failed to send assoc ret=%d", ret);
        association_ongoing = 0;
        s_assoc_generation = 0;
        memset(s_assoc_bssid, 0, MAC_ADDR_LEN);
        s_assoc_control_port = 0;
        if (!s_assoc_is_reassoc) {
            station_authorized = 0;
        }
        s_assoc_is_reassoc = 0;
        esp_wifi_unset_appie_internal(WIFI_APPIE_ASSOC_REQ);
        cmd_status = CMD_RESPONSE_FAIL;
        goto send_resp;
    }

send_resp:
    return send_command_resp(if_type, CMD_STA_ASSOC, cmd_status, NULL, 0);
}

int process_sta_set_authorized(uint8_t if_type, uint8_t *payload,
                               uint16_t payload_len)
{
    struct cmd_sta_set_authorized *cmd;
    uint8_t assoc_bssid[MAC_ADDR_LEN] = {0};
    const uint8_t *peer;
    uint8_t cmd_status = CMD_RESPONSE_SUCCESS;
    bool auth_done_failed = false;

    if (if_type != ESP_STA_IF || !payload || payload_len < sizeof(*cmd)) {
        ESP_LOGE(TAG,
                 "STA_PORT_AUTH_INVALID seq=%u if=%u payload_len=%u expected=%u",
                 active_cmd_seq, if_type, payload_len, (unsigned)sizeof(*cmd));
        cmd_status = CMD_RESPONSE_INVALID;
        goto send_resp;
    }

    cmd = (struct cmd_sta_set_authorized *)payload;
    if (cmd->authorized > 1) {
        ESP_LOGE(TAG, "STA_PORT_AUTH_INVALID seq=%u authorized=%u",
                 active_cmd_seq, cmd->authorized);
        cmd_status = CMD_RESPONSE_INVALID;
        goto send_resp;
    }

    ESP_LOGD(TAG,
             "STA_PORT_AUTH_RX seq=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x authorized=%u",
             active_cmd_seq, cmd->bssid[0], cmd->bssid[1], cmd->bssid[2],
             cmd->bssid[3], cmd->bssid[4], cmd->bssid[5], cmd->authorized);

    /* Deauthorizing an already-disconnected station is idempotently done. */
    if (!cmd->authorized && !station_connected) {
        station_authorized = 0;
        goto send_resp;
    }

    /* Host AUTHORIZED races WIFI_EVENT_STA_CONNECTED. Use the BSSID from
     * sta_connection() / assoc, not get_ap_info() which can still return
     * ESP_ERR_WIFI_NOT_CONNECT on an open network. */
    peer = ap_bssid;
    if (!peer) {
        if (esp_wifi_get_assoc_bssid_internal(assoc_bssid) != 0) {
            ESP_LOGW(TAG, "STA_PORT_AUTH_BUSY seq=%u no assoc BSSID yet",
                     active_cmd_seq);
            cmd_status = CMD_RESPONSE_BUSY;
            goto send_resp;
        }
        peer = assoc_bssid;
    }

    if (memcmp(cmd->bssid, zero_mac, MAC_ADDR_LEN) != 0 &&
        !IS_BROADCAST_ADDR(cmd->bssid) &&
        memcmp(peer, cmd->bssid, MAC_ADDR_LEN) != 0) {
        ESP_LOGE(TAG, "STA_PORT_AUTH_REJECT seq=%u stale/mismatched BSSID",
                 active_cmd_seq);
        cmd_status = CMD_RESPONSE_FAIL;
        goto send_resp;
    }

    if (cmd->authorized) {
        auth_done_failed = esp_wifi_auth_done_internal();
        ESP_LOGD(TAG, "STA_PORT_AUTH_DONE seq=%u ipc_failed=%u",
                 active_cmd_seq, auth_done_failed);
        if (auth_done_failed) {
            cmd_status = CMD_RESPONSE_FAIL;
        } else {
            station_authorized = 1;
        }
    } else {
        /* Deauthorization changes port authorization status without tearing
         * down the 802.11 association. Do not disconnect the peer. */
        ESP_LOGD(TAG, "STA_PORT_DEAUTH seq=%u", active_cmd_seq);
        station_authorized = 0;
        cmd_status = CMD_RESPONSE_SUCCESS;
    }

send_resp:
    return send_command_resp(if_type, CMD_STA_SET_AUTHORIZED, cmd_status, NULL, 0);
}

int process_sta_connect(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    struct cmd_sta_connect *cmd_connect;
    wifi_config_t wifi_config = {0};
    uint32_t type = 0;
    uint16_t cmd_status = CMD_RESPONSE_FAIL;

    if (!payload || payload_len < offsetof(struct cmd_sta_connect, assoc_ie)) {
        ESP_LOGE(TAG, "CONNECT_REJECT invalid fixed length=%u", payload_len);
        return send_command_resp(if_type, CMD_STA_CONNECT, CMD_RESPONSE_INVALID, NULL, 0);
    }
    cmd_connect = (struct cmd_sta_connect *) payload;
    if (!cmd_flex_len_valid(payload_len,
                            offsetof(struct cmd_sta_connect, assoc_ie),
                            cmd_connect->assoc_ie_len)) {
        ESP_LOGE(TAG, "CONNECT_REJECT invalid nested length payload=%u ie=%u",
                 payload_len, cmd_connect->assoc_ie_len);
        return send_command_resp(if_type, CMD_STA_CONNECT, CMD_RESPONSE_INVALID, NULL, 0);
    }

    type = (1 << WLAN_FC_STYPE_ASSOC_RESP)
           | (1 << WLAN_FC_STYPE_REASSOC_RESP)
           | (1 << WLAN_FC_STYPE_AUTH);
    esp_wifi_register_mgmt_frame_internal(type, 0);

    memcpy(wifi_config.sta.ssid, cmd_connect->ssid, MAX_SSID_LEN);
    if (!cmd_connect->is_auth_open) {
        ESP_LOGI(TAG, "Attempting secured connection");
        memcpy(wifi_config.sta.password, DUMMY_PASSPHRASE, sizeof(DUMMY_PASSPHRASE));
    } else {
        ESP_LOGI(TAG, "Attempting open connection");
    }
    memcpy(wifi_config.sta.bssid, cmd_connect->bssid, MAC_ADDR_LEN);
    wifi_config.sta.channel = cmd_connect->channel;
    ESP_LOGI(TAG, "%.*s, channel: %u", MAX_SSID_LEN,
             cmd_connect->ssid, cmd_connect->channel);

    if (cmd_connect->assoc_ie_len) {
        esp_wifi_unset_appie_internal(WIFI_APPIE_ASSOC_REQ);
        esp_wifi_set_appie_internal(WIFI_APPIE_ASSOC_REQ, cmd_connect->assoc_ie,
                                    cmd_connect->assoc_ie_len, 0);
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret) {
        ESP_LOGE(TAG, "Failed to set wifi config: %d\n", ret);
        goto send_resp;
    }

    ret = hosted_wifi_start();
    if (ret) {
        ESP_LOGE(TAG, "Failed to start wifi\n");
        goto send_resp;
    }

    association_ongoing = 1;
    s_assoc_generation = active_cmd_seq;
    memcpy(s_assoc_bssid, cmd_connect->bssid, MAC_ADDR_LEN);
    ret = esp_wifi_connect();
    if (ret) {
        ESP_LOGE(TAG, "Failed to connect wifi\n");
        association_ongoing = 0;
        s_assoc_generation = 0;
        memset(s_assoc_bssid, 0, MAC_ADDR_LEN);
        s_assoc_control_port = 0;
        s_assoc_is_reassoc = 0;
        goto send_resp;
    }

    cmd_status = CMD_RESPONSE_SUCCESS;
send_resp:
    ret = send_command_resp(if_type, CMD_STA_CONNECT, cmd_status, NULL, 0);

    return ret;
}

int process_deinit_interface(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;

    hosted_mgmt_fail_pending();
    wifi_mode_t wifi_mode = {0};
    uint16_t cmd_status = CMD_RESPONSE_SUCCESS;

    if (if_type == ESP_STA_IF) {
        if (sta_init_flag && esp_wifi_get_mode(&wifi_mode) == 0) {
            esp_wifi_deauthenticate_internal(WIFI_REASON_AUTH_LEAVE);
        }
        esp_wifi_disconnect();
    }
    esp_wifi_scan_stop();
    ret = hosted_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "hosted_wifi_stop failed ret=%d, forcing restart", ret);
        esp_restart();
    }

    s_assoc_generation = 0;
    memset(s_assoc_bssid, 0, MAC_ADDR_LEN);
    s_assoc_control_port = 0;
    s_assoc_is_reassoc = 0;
    s_auth_generation = 0;
    s_conn_generation = 0;
    s_disconnect_generation = 0;
    association_ongoing = 0;
    station_authorized = 0;

    ret = send_command_resp(if_type, CMD_DEINIT_INTERFACE, cmd_status, NULL, 0);

    return ret;
}

int process_init_interface(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    uint16_t cmd_status = CMD_RESPONSE_FAIL;

    /* s_wifi_started is updated synchronously by hosted_wifi_start/stop.
     * sta_init_flag / softap_started lag until WIFI_EVENT_*_START/STOP, so
     * they must not decide whether start is required after a fast DEINIT. */
    if (!s_wifi_started || (if_type == ESP_AP_IF && !softap_started)) {

        /* Use same MAC for AP and STA */
        esp_read_mac(dev_mac, ESP_MAC_WIFI_STA);

        if (if_type == ESP_AP_IF) {
            ESP_GOTO_ON_ERROR(esp_wifi_disconnect(), done, TAG, "Station Disconnect failed");
            ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), done, TAG, "Setting mode to APSTA failed");
            ESP_LOGI(TAG, "Setting APSTA mode");
            wifi_config_t wifi_config = {0};
            ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), done, TAG, "Set config for sta failed");
        } else if (if_type == ESP_STA_IF) {
            ESP_LOGI(TAG, "Setting STA mode");
            ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), done, TAG, "Setting mode to STA failed");
        } else {
            ESP_LOGE(TAG, "Invalid interface type");
            goto done;
        }

        ESP_GOTO_ON_ERROR(hosted_wifi_start(), done, TAG, "Failed to start Wi-Fi");

    }
    cmd_status = CMD_RESPONSE_SUCCESS;
done:
    ret = send_command_resp(if_type, CMD_INIT_INTERFACE, cmd_status, NULL, 0);
    if (ret != ESP_OK) {
        deinitialize_wifi();
    }

    return ret;
}

int process_get_mac(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    (void)payload;
    (void)payload_len;
    esp_err_t ret = ESP_OK;
    uint8_t cmd_status = CMD_RESPONSE_SUCCESS;

    /*ESP_LOG_BUFFER_HEXDUMP(TAG, mac, MAC_ADDR_LEN, ESP_LOG_INFO);*/
    ret = send_command_resp(if_type, CMD_GET_MAC, cmd_status, dev_mac, MAC_ADDR_LEN);
    return ret;
}

int process_set_mac(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    wifi_interface_t wifi_if_type = 0;
    uint8_t cmd_status = CMD_RESPONSE_SUCCESS;
    struct cmd_config_mac_address *mac = (struct cmd_config_mac_address *) payload;

    if (if_type == ESP_STA_IF) {
        wifi_if_type = WIFI_IF_STA;
    } else {
        wifi_if_type = WIFI_IF_AP;
    }
    ESP_LOGI(TAG, "Setting mac address \n");
    ret = esp_wifi_set_mac(wifi_if_type, mac->mac_addr);

    if (ret) {
        ESP_LOGE(TAG, "Failed to set mac address\n");
        cmd_status = CMD_RESPONSE_FAIL;
    } else {
        memcpy(dev_mac, mac->mac_addr, MAC_ADDR_LEN);
    }
    ret = send_command_resp(if_type, CMD_SET_MAC, cmd_status, (uint8_t *)mac->mac_addr,
                            MAC_ADDR_LEN);
    return ret;
}

int process_set_default_key(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    struct cmd_key_operation *cmd = NULL;
    uint16_t cmd_status;

    /*ESP_LOGI(TAG, "%s:%u\n", __func__, __LINE__);*/

    if (!payload || payload_len < sizeof(*cmd)) {
        ESP_LOGE(TAG, "Default-key command invalid length=%u", payload_len);
        return send_command_resp(if_type, CMD_SET_DEFAULT_KEY, CMD_RESPONSE_INVALID, NULL, 0);
    }
    cmd = (struct cmd_key_operation *) payload;
    if (!cmd) {
        ESP_LOGE(TAG, "%s:%u command failed\n", __func__, __LINE__);
        cmd_status = CMD_RESPONSE_FAIL;
        goto SEND_CMD;
    }

    /* Firmware just responds to this request as success. */
    cmd_status = CMD_RESPONSE_SUCCESS;

SEND_CMD:
    ret = send_command_resp(if_type, CMD_SET_DEFAULT_KEY, cmd_status, NULL, 0);

    return ret;
}

int process_del_key(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    struct wifi_sec_key * key = NULL;
    struct cmd_key_operation *cmd = NULL;
    uint16_t cmd_status = CMD_RESPONSE_SUCCESS;

    /*ESP_LOGI(TAG, "%s:%u\n", __func__, __LINE__);*/

    if (!payload || payload_len < sizeof(*cmd)) {
        ESP_LOGE(TAG, "Delete-key command invalid length=%u", payload_len);
        return send_command_resp(if_type, CMD_DEL_KEY, CMD_RESPONSE_INVALID, NULL, 0);
    }
    cmd = (struct cmd_key_operation *) payload;

    if (!cmd) {
        ESP_LOGE(TAG, "%s:%u command failed\n", __func__, __LINE__);
        cmd_status = CMD_RESPONSE_FAIL;
        goto send_resp;
    }

    key = &cmd->key;

    if (!key->del) {
        ESP_LOGE(TAG, "%s:%u command failed\n", __func__, __LINE__);
        cmd_status = CMD_RESPONSE_FAIL;
        goto send_resp;
    }

send_resp:
    ret = send_command_resp(if_type, CMD_DEL_KEY, cmd_status, NULL, 0);

    return ret;
}

// hw_index = 8 for AP aid = 0
void esp_wifi_get_and_print_key(u8 hw_index)
{
    u8 idx;
    int algo;
    u8 addr[6];
    int keyindex;
    u8 key1[32];

    int ic_get_key(u8 * idx, int *algo, int *keyindex, u8 * addr, u8 hw_index, u8 * key, u8 key_len);
    ic_get_key(&idx, &algo, &keyindex, addr, hw_index, key1, 32);

    ESP_LOGI(TAG, "index=%d, algo=%d, keyindex=%d", idx, algo, keyindex);
    ESP_LOG_BUFFER_HEXDUMP("mac", addr, 6, ESP_LOG_INFO);
    ESP_LOG_BUFFER_HEXDUMP("key", key1, 32, ESP_LOG_INFO);
}

static int set_key_internal(void *data)
{
    wifi_interface_t iface = WIFI_IF_STA;
    struct cmd_key_operation *cmd = NULL;
    struct wifi_sec_key * key = NULL;
    uint8_t if_type;
	int ret = ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "%s:%u\n", __func__, __LINE__);

    cmd = (struct cmd_key_operation *) data;
    if (!cmd) {
        ESP_LOGE(TAG, "%s:%u command failed\n", __func__, __LINE__);
        return ESP_ERR_INVALID_ARG;
    }

	if_type = cmd->header.reserved1;
	if (if_type == ESP_AP_IF)
		iface = WIFI_IF_AP;

    key = &cmd->key;
    uint32_t key_len = le32toh(key->len);
    uint32_t seq_len = le32toh(key->seq_len);
    uint32_t algo = le32toh(key->algo);
    uint32_t index = le32toh(key->index);

    if (key_len > sizeof(key->data) || seq_len > sizeof(key->seq)) {
        ESP_LOGE(TAG, "Invalid key material lengths key=%"PRIu32"/%u seq=%"PRIu32"/%u",
                 key_len, (unsigned)sizeof(key->data),
                 seq_len, (unsigned)sizeof(key->seq));
        goto out;
    }

    if (algo == WIFI_WPA_ALG_WEP40 || algo == WIFI_WPA_ALG_WEP104) {
        ret = ESP_OK;
        goto out;
    }
    if (index) {
        if (algo == WIFI_WPA_ALG_IGTK) {
            wifi_wpa_igtk_t igtk = {0};

            if (key_len > sizeof(igtk.igtk) || seq_len > sizeof(igtk.pn)) {
                ESP_LOGE(TAG, "Invalid IGTK material lengths key=%"PRIu32"/%u seq=%"PRIu32"/%u",
                         key_len, (unsigned)sizeof(igtk.igtk),
                         seq_len, (unsigned)sizeof(igtk.pn));
                goto out;
            }
            ESP_LOGI(TAG, "Setting iGTK [%"PRIu32"]\n", index);

            memcpy(igtk.igtk, key->data, key_len);
            memcpy(igtk.pn, key->seq, seq_len);
            WPA_PUT_LE16(igtk.keyid, index);
            ret = esp_wifi_set_igtk_internal(iface, &igtk);
        } else {
            /* GTK */
            ESP_LOGI(TAG, "Setting GTK [%"PRIu32"]\n", index);
            if (iface == WIFI_IF_AP) {
                ret = esp_wifi_set_ap_key_internal(algo, key->mac_addr, index,
                                                   key->data, key_len);
            } else {
                ret = esp_wifi_set_sta_key_internal(algo, key->mac_addr, index,
                                                    0, key->seq, seq_len, key->data, key_len,
                                                    KEY_FLAG_GROUP | KEY_FLAG_RX);
            }
        }
    } else {
        /* PTK */
        ESP_LOGI(TAG, "Setting PTK algo=%"PRIu32" index=%"PRIu32, algo, index);
        ESP_LOG_BUFFER_HEXDUMP("mac", key->mac_addr, 6, ESP_LOG_DEBUG);
        ESP_LOG_BUFFER_HEXDUMP("key", key->data, key_len, ESP_LOG_DEBUG);
        if (iface == WIFI_IF_AP) {
            ret = esp_wifi_set_ap_key_internal(algo, key->mac_addr, index,
                                               key->data, key_len);
        } else {
            ret = esp_wifi_set_sta_key_internal(algo, key->mac_addr, index,
                                                1, key->seq, seq_len, key->data, key_len,
                                                KEY_FLAG_PAIRWISE | KEY_FLAG_RX | KEY_FLAG_TX);
        }
    }

    if (ret) {
        ESP_LOGE(TAG, "%s:%u driver key set ret=%d\n", __func__, __LINE__, ret);
    }

out:
    free(cmd);

    return ret;
}

static int wifi_set_keys(void *args)
{
    wifi_ipc_config_t cfg;

    cfg.fn = set_key_internal;
    cfg.arg = args;
    cfg.arg_size = 0;
    return esp_wifi_ipc_internal(&cfg, true);
}

int process_add_key(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    struct cmd_key_operation *cmd = NULL;
    uint8_t cmd_status;

    if (!payload || payload_len < sizeof(*cmd)) {
        ESP_LOGE(TAG, "Add-key command invalid length=%u", payload_len);
        return send_command_resp(if_type, CMD_ADD_KEY, CMD_RESPONSE_INVALID, NULL, 0);
    }
    cmd = (struct cmd_key_operation *)payload;
    if (le32toh(cmd->key.len) > sizeof(cmd->key.data) ||
        le32toh(cmd->key.seq_len) > sizeof(cmd->key.seq)) {
        ESP_LOGE(TAG, "Add-key invalid material length key=%lu seq=%lu",
                 (unsigned long)le32toh(cmd->key.len), (unsigned long)le32toh(cmd->key.seq_len));
        return send_command_resp(if_type, CMD_ADD_KEY, CMD_RESPONSE_INVALID, NULL, 0);
    }

    cmd = malloc(sizeof(*cmd));
    if (!cmd) {
        ESP_LOGE(TAG, "%s:%u memory allocation failed\n", __func__, __LINE__);
        cmd_status = CMD_RESPONSE_FAIL;
        goto SEND_CMD;
    }
    memcpy(cmd, payload, sizeof(*cmd));
    cmd->header.reserved1 = if_type;

    cmd_status = CMD_RESPONSE_SUCCESS;
    ret = wifi_set_keys(cmd);
    if (ret) {
        cmd_status = CMD_RESPONSE_FAIL;
    }

SEND_CMD:
    ret = send_command_resp(if_type, CMD_ADD_KEY, cmd_status, NULL, 0);
    return ret;
}

int process_set_mode(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    uint8_t cmd_status = CMD_RESPONSE_FAIL;
    struct cmd_config_mode *mode;
    wifi_mode_t old_mode = WIFI_MODE_NULL;
    uint8_t old_sta_mac[MAC_ADDR_LEN] = {0};
    uint8_t old_ap_mac[MAC_ADDR_LEN] = {0};
    bool was_wifi_started;
    uint16_t req_mode;
    bool need_ap_cb;

    if (!payload || payload_len < sizeof(*mode))
        return send_command_resp(if_type, CMD_SET_MODE, CMD_RESPONSE_INVALID, NULL, 0);

    mode = (struct cmd_config_mode *) payload;
    req_mode = le16toh(mode->mode);
    if (req_mode != WIFI_MODE_STA && req_mode != WIFI_MODE_AP &&
        req_mode != WIFI_MODE_APSTA) {
        return send_command_resp(if_type, CMD_SET_MODE, CMD_RESPONSE_INVALID, NULL, 0);
    }
    need_ap_cb = (req_mode == WIFI_MODE_AP || req_mode == WIFI_MODE_APSTA);

    ret = esp_wifi_get_mode(&old_mode);
    if (ret) {
        ESP_LOGE(TAG, "Failed to get mode");
        goto send_err;
    }

    if (old_mode == req_mode && s_wifi_started &&
        (!need_ap_cb || s_mgmt_cb_owned)) {
        ESP_LOGI(TAG, "old mode and new modes are same, already running");
        goto send_ok;
    }

    /* Snapshot state before committing to transition */
    wifi_config_t old_sta_config = {0};
    wifi_config_t old_ap_config = {0};
    bool had_sta_config = (esp_wifi_get_config(WIFI_IF_STA, &old_sta_config) == ESP_OK);
    bool had_ap_config = (esp_wifi_get_config(WIFI_IF_AP, &old_ap_config) == ESP_OK);
    bool was_mgmt_cb_owned = s_mgmt_cb_owned;
    (void)esp_wifi_get_mac(WIFI_IF_STA, old_sta_mac);
    (void)esp_wifi_get_mac(WIFI_IF_AP, old_ap_mac);
    was_wifi_started = s_wifi_started;

    if (old_mode != req_mode) {
        ret = hosted_wifi_stop();
        if (ret) {
            ESP_LOGE(TAG, "Failed to stop wifi ret=%d\n", ret);
            goto send_err;
        }

        if (req_mode == WIFI_MODE_AP || req_mode == WIFI_MODE_APSTA) {
            ESP_LOGI(TAG, "Setting APSTA mode");
            ESP_GOTO_ON_ERROR(esp_wifi_set_mac(WIFI_IF_STA, dummy_mac), rollback, TAG, "Setting MAC on STA failed");
            ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), rollback, TAG, "Setting mode to APSTA failed");
            ESP_GOTO_ON_ERROR(esp_wifi_set_mac(WIFI_IF_AP, dev_mac), rollback, TAG, "Setting MAC on AP failed");
            wifi_config_t wifi_config = {0};
            ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), rollback, TAG, "Set config for sta failed");
        } else if (req_mode == WIFI_MODE_STA) {
            ESP_LOGI(TAG, "Setting STA mode");
            ESP_GOTO_ON_ERROR(esp_wifi_set_mac(WIFI_IF_AP, dummy_mac2), rollback, TAG, "Setting MAC on AP failed");
            ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), rollback, TAG, "Setting mode to STA failed");
            ESP_GOTO_ON_ERROR(esp_wifi_set_mac(WIFI_IF_STA, dev_mac), rollback, TAG, "Setting MAC on STA failed");
        }
    }

    if (need_ap_cb) {
        ret = mgmt_tx_ensure_cb();
        if (ret)
            goto rollback;
    }
    if (!s_wifi_started)
        ESP_GOTO_ON_ERROR(hosted_wifi_start(), rollback, TAG, "Cannot start wifi");

    /* Transition committed: abort pending MGMT TX */
    hosted_mgmt_fail_pending();

send_ok:
    cmd_status = CMD_RESPONSE_SUCCESS;
    goto send_resp;

rollback:
    {
        esp_err_t rb_err = ESP_OK;
        rb_err |= esp_wifi_set_mode(old_mode);
        rb_err |= esp_wifi_set_mac(WIFI_IF_STA, old_sta_mac);
        rb_err |= esp_wifi_set_mac(WIFI_IF_AP, old_ap_mac);
        if (had_sta_config)
            rb_err |= esp_wifi_set_config(WIFI_IF_STA, &old_sta_config);
        if (had_ap_config)
            rb_err |= esp_wifi_set_config(WIFI_IF_AP, &old_ap_config);
        if (!was_mgmt_cb_owned && s_mgmt_cb_owned) {
            pp_unregister_tx_cb(WIFI_TXCB_MGMT_ID);
            s_mgmt_cb_owned = false;
        }
        if (was_wifi_started && !s_wifi_started)
            rb_err |= hosted_wifi_start();
        else if (!was_wifi_started && s_wifi_started)
            rb_err |= hosted_wifi_stop();

        if (rb_err != ESP_OK) {
            ESP_LOGE(TAG, "SET_MODE rollback failed (rb_err=%d), forcing restart", rb_err);
            esp_restart();
        }
    }

send_err:
    cmd_status = CMD_RESPONSE_FAIL;
send_resp:
    {
        uint16_t resp_mode = htole16(req_mode);
        return send_command_resp(if_type, CMD_SET_MODE, cmd_status, (uint8_t *)&resp_mode,
                                sizeof(uint16_t));
    }
}

int process_set_ie(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    uint8_t cmd_status = CMD_RESPONSE_SUCCESS;
    struct cmd_config_ie *ie;
    uint16_t ie_len;
    int type = 0;

    if (!payload || payload_len < offsetof(struct cmd_config_ie, ie)) {
        return send_command_resp(if_type, CMD_SET_IE, CMD_RESPONSE_INVALID, NULL, 0);
    }
    ie = (struct cmd_config_ie *) payload;
    ie_len = le16toh(ie->ie_len);
    if (!cmd_flex_len_valid(payload_len,
                            offsetof(struct cmd_config_ie, ie), ie_len)) {
        ESP_LOGE(TAG, "IE_REJECT invalid nested length payload=%u ie=%u",
                 payload_len, ie_len);
        return send_command_resp(if_type, CMD_SET_IE, CMD_RESPONSE_INVALID, NULL, 0);
    }

    /* Zero-length removal is specialized. */
    if (ie_len == 0) {
        int unset_type;

        if (if_type != ESP_AP_IF)
            return send_command_resp(if_type, CMD_SET_IE, CMD_RESPONSE_INVALID, NULL, 0);

        switch (ie->ie_type) {
        case IE_BEACON:
            unset_type = WIFI_APPIE_RAM_BEACON;
            break;
        case IE_BEACON_PROBE_HEAD:
            unset_type = WIFI_APPIE_RAM_BEACON_PROBE_HEAD;
            break;
        case IE_BEACON_PROBE_TAIL:
            unset_type = WIFI_APPIE_RAM_BEACON_PROBE_TAIL;
            break;
        case IE_PROBE_RESP:
            unset_type = WIFI_APPIE_RAM_PROBE_RSP;
            break;
        case IE_ASSOC_RESP:
            unset_type = WIFI_APPIE_ASSOC_RESP;
            break;
        default:
            return send_command_resp(if_type, CMD_SET_IE, CMD_RESPONSE_INVALID, NULL, 0);
        }

        ret = esp_wifi_unset_appie_internal(unset_type);
        cmd_status = ret == ESP_OK ? CMD_RESPONSE_SUCCESS : CMD_RESPONSE_INVALID;
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "Unset IE type=%d failed ret=%d", ie->ie_type, ret);
        return send_command_resp(if_type, CMD_SET_IE, cmd_status, NULL, 0);
    }

    ESP_LOGI(TAG, "Setting IE type=%d len=%d\n", ie->ie_type, ie_len);

    if (if_type != ESP_AP_IF) {
        cmd_status = CMD_RESPONSE_INVALID;
        goto send_resp;
    }
    if (ie->ie_type == IE_BEACON) {
        type = WIFI_APPIE_RAM_BEACON;
        ESP_LOG_BUFFER_HEXDUMP("BEACON", (uint8_t *) ie->ie, ie_len, ESP_LOG_INFO);
    } else if (ie->ie_type == IE_BEACON_PROBE_HEAD) {
        type = WIFI_APPIE_RAM_BEACON_PROBE_HEAD;
        ESP_LOG_BUFFER_HEXDUMP("BEACON_PROBE_HEAD", (uint8_t *) ie->ie, ie_len, ESP_LOG_INFO);
    } else if (ie->ie_type == IE_BEACON_PROBE_TAIL) {
        type = WIFI_APPIE_RAM_BEACON_PROBE_TAIL;
        ESP_LOG_BUFFER_HEXDUMP("BEACON_PROBE_TAIL", (uint8_t *) ie->ie, ie_len, ESP_LOG_INFO);
    } else if (ie->ie_type == IE_PROBE_RESP) {
        type = WIFI_APPIE_RAM_PROBE_RSP;
        ESP_LOG_BUFFER_HEXDUMP("PROBE", (uint8_t *) ie->ie, ie_len, ESP_LOG_INFO);
    } else if (ie->ie_type == IE_ASSOC_RESP) {
        type = WIFI_APPIE_ASSOC_RESP;
    } else {
        cmd_status = CMD_RESPONSE_INVALID;
        goto send_resp;
    }

    ret = esp_wifi_set_appie_internal(type, ie->ie, ie_len, 0);

    if (ret) {
        cmd_status = CMD_RESPONSE_INVALID;
    }
send_resp:
    ret = send_command_resp(if_type, CMD_SET_IE, cmd_status, NULL, 0);

    return ret;
}

uint8_t *esp_wifi_get_eb_data(void *eb);
uint32_t esp_wifi_get_eb_data_len(void *eb);

void ieee80211_tx_mgt_cb(void *eb);

static void mgmt_txcb_unmarked(void *eb)
{
    uint8_t cmd_status = CMD_RESPONSE_FAIL;
    uint8_t *data;
    uint32_t len;
    const uint8_t *frame = NULL;
    uint32_t frame_len = 0;
    uint8_t *frame_copy = NULL;
    uint16_t seq = 0;
    bool had_inflight;
    bool valid_frame = false;

    data = esp_wifi_get_eb_data(eb);
    len = esp_wifi_get_eb_data_len(eb);

    /* Locate the 802.11 header. HE TX-done blobs are prefixed; try both. */
    if (data && len >= (ETH_ALEN + 4) && mgmt_tx_hdr_matches(data, len)) {
        frame = data;
        frame_len = len;
    } else if (data && len > 8 && mgmt_tx_hdr_matches(data + 8, len - 8)) {
        frame = data + 8;
        frame_len = len - 8;
    } else if (data && len >= TX_DONE_PREFIX + ETH_ALEN + 4) {
        frame = data + TX_DONE_PREFIX;
        frame_len = len - TX_DONE_PREFIX;
    }

    /* Broadcast TX already completed the host command immediately. A late
     * callback must not take() a newer unicast sequence. */
    if (frame && frame_len >= (ETH_ALEN + 4) && IS_BROADCAST_ADDR(frame + 4)) {
        ieee80211_tx_mgt_cb(eb);
        return;
    }

    had_inflight = mgmt_tx_inflight_take_matching(data, len, &seq);

    if (esp_wifi_eb_tx_status_success_internal(eb))
        cmd_status = CMD_RESPONSE_SUCCESS;
    if (frame && frame_len >= (ETH_ALEN + 4) && !IS_BROADCAST_ADDR(frame + 4)) {
        valid_frame = true;
        frame_copy = malloc(frame_len);
        if (frame_copy) {
            memcpy(frame_copy, frame, frame_len);
        } else {
            ESP_LOGE(TAG, "%s: failed to copy TX-done frame len=%"PRIu32,
                     __func__, frame_len);
            cmd_status = CMD_RESPONSE_FAIL;
        }
    }

    /* Private Wi-Fi may recycle eb here. Only the copy is used afterward. */
    ieee80211_tx_mgt_cb(eb);

    if (!had_inflight) {
        free(frame_copy);
        return;
    }

    if (valid_frame && frame_copy) {
        if (send_mgmt_tx_done(cmd_status, WIFI_IF_AP, frame_copy, frame_len, seq) != ESP_OK)
            send_mgmt_tx_done(CMD_RESPONSE_FAIL, WIFI_IF_AP, NULL, 0, seq);
    } else {
        send_mgmt_tx_done(CMD_RESPONSE_FAIL, WIFI_IF_AP, NULL, 0, seq);
    }

    free(frame_copy);
}

esp_err_t wlan_ap_rx_callback(void *buffer, uint16_t len, void *eb);
static void mgmt_txcb(void *eb)
{
    uint8_t *data;
    uint32_t len;
    bool tx_ack;
    bool hosted_pending;
    uint64_t cookie = 0;
    uint8_t *frame = NULL;
    uint32_t frame_len = 0;
    bool report_status = false;
    TimerHandle_t timer = NULL;
    bool finish = false;

    /* Old-host requests use process_mgmt_tx_unmarked(), whose completion state
     * is owned by the legacy callback. Delegate when no new async transaction
     * is active so rolling old-host/new-firmware operation remains intact. */
    portENTER_CRITICAL(&s_hosted_mgmt_lock);
    if (s_hosted_mgmt_draining) {
        s_hosted_mgmt_draining = false;
        portEXIT_CRITICAL(&s_hosted_mgmt_lock);
        ieee80211_tx_mgt_cb(eb);
        return;
    }
    hosted_pending = s_hosted_mgmt.pending;
    portEXIT_CRITICAL(&s_hosted_mgmt_lock);
    if (!hosted_pending) {
        mgmt_txcb_unmarked(eb);
        return;
    }

    data = esp_wifi_get_eb_data(eb);
    len = esp_wifi_get_eb_data_len(eb);
    tx_ack = esp_wifi_eb_tx_status_success_internal(eb);

    portENTER_CRITICAL(&s_hosted_mgmt_lock);
    if (hosted_mgmt_data_matches_locked(data, len)) {
        if (!s_hosted_mgmt.accepted) {
            s_hosted_mgmt.completed = true;
            s_hosted_mgmt.ack = tx_ack;
        } else {
            cookie = s_hosted_mgmt.cookie;
            frame = s_hosted_mgmt.frame;
            frame_len = s_hosted_mgmt.frame_len;
            report_status = s_hosted_mgmt.report_status;
            timer = s_hosted_mgmt.timer;
            memset(&s_hosted_mgmt, 0, sizeof(s_hosted_mgmt));
            finish = true;
        }
    }
    portEXIT_CRITICAL(&s_hosted_mgmt_lock);

    ieee80211_tx_mgt_cb(eb);

    if (finish)
        hosted_mgmt_finish_owned(cookie, frame, frame_len, tx_ack,
                                 report_status, timer);
}

int process_set_ap_config(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    uint8_t cmd_status = CMD_RESPONSE_SUCCESS;
    struct cmd_ap_config *ap_config;
    wifi_mode_t old_mode = WIFI_MODE_NULL;
    wifi_config_t old_ap_config = {0};
    bool was_wifi_started;

    if (if_type != ESP_AP_IF || !payload || payload_len < sizeof(*ap_config)) {
        cmd_status = CMD_RESPONSE_INVALID;
        goto send_resp;
    }

    ap_config = (struct cmd_ap_config *) payload;
    if (ap_config->ap_config.ssid_len > sizeof(ap_config->ap_config.ssid)) {
        ESP_LOGE(TAG, "AP config invalid SSID length=%u max=%u",
                 ap_config->ap_config.ssid_len,
                 (unsigned)sizeof(ap_config->ap_config.ssid));
        cmd_status = CMD_RESPONSE_INVALID;
        goto send_resp;
    }

    wifi_config_t wifi_config = {0};
    uint16_t beacon_interval = le16toh(ap_config->ap_config.beacon_interval);
    uint16_t inactivity_timeout = le16toh(ap_config->ap_config.inactivity_timeout);

    memcpy(wifi_config.ap.ssid, ap_config->ap_config.ssid, ap_config->ap_config.ssid_len);
    /* set dummy config for data path */
    if (ap_config->ap_config.privacy) {
        memcpy(wifi_config.ap.password, "12345678", 8);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    }
    wifi_config.ap.ssid_len = ap_config->ap_config.ssid_len;
    wifi_config.ap.channel = ap_config->ap_config.channel;
    wifi_config.ap.ssid_hidden = ap_config->ap_config.ssid_hidden;
    wifi_config.ap.beacon_interval = beacon_interval;

    ESP_LOGI(TAG, "ap config ssid=%.*s ssid_len=%d channel=%d authmode=%d hidden=%d bi=%d cipher=%d\n",
             wifi_config.ap.ssid_len, wifi_config.ap.ssid, wifi_config.ap.ssid_len,
             wifi_config.ap.channel, wifi_config.ap.authmode,
             wifi_config.ap.ssid_hidden, wifi_config.ap.beacon_interval,
             wifi_config.ap.pairwise_cipher);

    wifi_config.ap.max_connection = 8;

    ret = esp_wifi_get_mode(&old_mode);
    if (ret) {
        ESP_LOGE(TAG, "Failed to get mode");
        cmd_status = CMD_RESPONSE_FAIL;
        goto send_resp;
    }

    uint16_t old_inactive_time = 0;
    bool had_inactivity_time = (esp_wifi_get_inactive_time(WIFI_IF_AP, &old_inactive_time) == ESP_OK);
    bool was_mgmt_cb_owned = s_mgmt_cb_owned;
    (void)esp_wifi_get_config(WIFI_IF_AP, &old_ap_config);
    was_wifi_started = s_wifi_started;

    ret = hosted_wifi_stop();
    if (ret) {
        ESP_LOGE(TAG, "Failed to stop wifi before AP config ret=%d", ret);
        cmd_status = CMD_RESPONSE_FAIL;
        goto send_resp;
    }

    if (old_mode != WIFI_MODE_APSTA) {
        ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ret) {
            ESP_LOGE(TAG, "Failed to set APSTA mode ret=%d", ret);
            goto rollback;
        }
    }
    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret) {
        ESP_LOGE(TAG, "Failed to set AP config ret=%d", ret);
        goto rollback;
    }

    if (inactivity_timeout) {
        ESP_LOGI(TAG, "inactivity_timeout=%d\n", inactivity_timeout);
        ret = esp_wifi_set_inactive_time(WIFI_IF_AP, inactivity_timeout);
        if (ret) {
            ESP_LOGE(TAG, "Failed to set AP inactivity timeout ret=%d", ret);
            goto rollback;
        }
    }
    ret = mgmt_tx_ensure_cb();
    if (ret) {
        goto rollback;
    }
    ret = esp_wifi_internal_reg_rxcb(WIFI_IF_AP, (wifi_rxcb_t)wlan_ap_rx_callback);
    if (ret) {
        ESP_LOGE(TAG, "Failed to register AP RX callback ret=%d", ret);
        goto rollback;
    }
    ret = hosted_wifi_start();
    if (ret) {
        ESP_LOGE(TAG, "Failed to start AP Wi-Fi ret=%d", ret);
        goto rollback;
    }

    /* Transition committed: abort pending MGMT TX */
    hosted_mgmt_fail_pending();

    goto send_resp;

rollback:
    cmd_status = CMD_RESPONSE_FAIL;
    {
        esp_err_t rb_err = ESP_OK;
        rb_err |= esp_wifi_set_mode(old_mode);
        rb_err |= esp_wifi_set_config(WIFI_IF_AP, &old_ap_config);
        if (had_inactivity_time)
            rb_err |= esp_wifi_set_inactive_time(WIFI_IF_AP, old_inactive_time);
        if (!was_mgmt_cb_owned && s_mgmt_cb_owned) {
            pp_unregister_tx_cb(WIFI_TXCB_MGMT_ID);
            s_mgmt_cb_owned = false;
        }
        if (!was_wifi_started) {
            esp_wifi_internal_reg_rxcb(WIFI_IF_AP, NULL);
        }
        if (was_wifi_started && !s_wifi_started)
            rb_err |= hosted_wifi_start();
        else if (!was_wifi_started && s_wifi_started)
            rb_err |= hosted_wifi_stop();

        if (rb_err != ESP_OK) {
            ESP_LOGE(TAG, "AP_CONFIG rollback failed (rb_err=%d), forcing restart", rb_err);
            esp_restart();
        }
    }

send_resp:
    ret = send_command_resp(if_type, CMD_AP_CONFIG, cmd_status, NULL, 0);

    return ret;
}

static int send_mgmt_tx_done(uint8_t cmd_status, wifi_interface_t wifi_if_type, uint8_t *data, uint32_t len, uint16_t seq)
{
    interface_buffer_handle_t buf_handle = {0};
    struct cmd_mgmt_tx *header;
    esp_err_t ret = ESP_OK;

    if (wifi_if_type == WIFI_IF_AP) {
        buf_handle.if_type = ESP_AP_IF;
    } else {
        buf_handle.if_type = ESP_STA_IF;
    }
    buf_handle.if_num = 0;
	if ((len && !data) || len > UINT16_MAX - sizeof(struct cmd_mgmt_tx)) {
		ESP_LOGE(TAG, "MGMT_TX_DONE invalid data=%p len=%"PRIu32, data, len);
		return ESP_ERR_INVALID_ARG;
	}
    buf_handle.payload_len = sizeof(struct cmd_mgmt_tx) + len;
    buf_handle.pkt_type = PACKET_TYPE_COMMAND_RESPONSE;

    buf_handle.payload = heap_caps_malloc(buf_handle.payload_len, MALLOC_CAP_DMA);
	if (!buf_handle.payload) {
		ESP_LOGE(TAG, "MGMT_TX_DONE allocation failed len=%u", buf_handle.payload_len);
		return ESP_ERR_NO_MEM;
	}
    memset(buf_handle.payload, 0, buf_handle.payload_len);

    header = (struct cmd_mgmt_tx *) buf_handle.payload;

    header->header.cmd_code = CMD_MGMT_TX;
    header->header.len = 0;
    header->header.cmd_status = cmd_status;
    header->header.seq_num = htole16(seq);
    ESP_LOGD(TAG, "CMD_RESP_TX code=%u seq=%u status=%u len=%"PRIu32,
             CMD_MGMT_TX, seq, cmd_status, len);
    header->len = len;
    if (data && len)
        memcpy(header->buf, data, len);

    buf_handle.priv_buffer_handle = buf_handle.payload;
    buf_handle.free_buf_handle = free;

    ret = send_command_response(&buf_handle);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send command response\n");
        goto DONE;
    }

    return ESP_OK;
DONE:
    if (buf_handle.payload) {
        free(buf_handle.payload);
        buf_handle.payload = NULL;
    }

    return ret;
}

int ieee80211_send_mgmt_internal(wifi_interface_t wifi_if_type, uint8_t *buf, size_t len);

static int process_mgmt_tx_unmarked(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    esp_err_t ret = ESP_OK;
    wifi_interface_t wifi_if_type = WIFI_IF_AP;
    uint8_t cmd_status = CMD_RESPONSE_SUCCESS;
    struct cmd_mgmt_tx_unmarked *mgmt_tx;
    uint16_t seq;

    if (!payload || payload_len < offsetof(struct cmd_mgmt_tx_unmarked, buf)) {
        return send_command_resp(if_type, CMD_MGMT_TX, CMD_RESPONSE_INVALID, NULL, 0);
    }
    mgmt_tx = (struct cmd_mgmt_tx_unmarked *) payload;
    seq = le16toh(mgmt_tx->header.seq_num);

    if (!cmd_flex_len_valid(payload_len, offsetof(struct cmd_mgmt_tx_unmarked, buf),
                            mgmt_tx->len) || mgmt_tx->len < 10) {
        ESP_LOGE(TAG, "MGMT_TX_REJECT invalid nested length payload=%u frame=%"PRIu32,
                 payload_len, mgmt_tx->len);
        cmd_status = CMD_RESPONSE_INVALID;
        goto send_resp;
    }

    if (if_type != ESP_AP_IF || !softap_started) {
        ESP_LOGE(TAG, "%s: err on wrong interface=%d\n", __func__, if_type);
        cmd_status = CMD_RESPONSE_INVALID;
        wifi_if_type = ESP_STA_IF;
        goto send_resp;
    }

    ret = mgmt_tx_ensure_cb();
    if (ret) {
        cmd_status = CMD_RESPONSE_FAIL;
        goto send_resp;
    }

    if (!mgmt_tx_inflight_begin(seq, mgmt_tx->buf, mgmt_tx->len)) {
        ESP_LOGW(TAG, "MGMT_TX_BUSY seq=%u, async TX already pending", seq);
        cmd_status = CMD_RESPONSE_BUSY;
        goto send_resp;
    }

    ret = ieee80211_send_mgmt_internal(WIFI_IF_AP, mgmt_tx->buf, mgmt_tx->len);

    if (ret) {
        mgmt_tx_inflight_take(NULL);
        cmd_status = CMD_RESPONSE_INVALID;
        goto send_resp;
    }

    if (IS_BROADCAST_ADDR(mgmt_tx->buf + 4)) {
        ESP_LOGI(TAG, "%s: broadcast address, sending response immediately\n", __func__);
        mgmt_tx_inflight_take(NULL);
        goto send_resp;
    }
    /* send response in separate ctx once done */
    return 0;
send_resp:
    return send_mgmt_tx_done(cmd_status, wifi_if_type, NULL, 0, seq);
}

int process_mgmt_tx(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    struct cmd_mgmt_tx *mgmt_tx;
    uint32_t frame_len;
    uint64_t cookie;
    uint16_t seq;
    bool broadcast;
    bool track;
    esp_err_t ret;

    if (!payload || payload_len < offsetof(struct cmd_mgmt_tx, buf))
        return hosted_send_mgmt_accept(CMD_RESPONSE_INVALID, active_cmd_seq);

    mgmt_tx = (struct cmd_mgmt_tx *)payload;

    /* No marker means an old host. Preserve the exact frozen deferred-response
     * behavior, including its old completion callback contract. */
    if (mgmt_tx->header.reserved1 != HOSTED_MGMT_TX_ASYNC_STATUS_V1)
        return process_mgmt_tx_unmarked(if_type, payload, payload_len);

    seq = le16toh(mgmt_tx->header.seq_num);
    frame_len = le32toh(mgmt_tx->len);
    cookie = le64toh(mgmt_tx->mgmt_tx_id);
    if (!cookie)
        cookie = seq;

    if (!cmd_flex_len_valid(payload_len, offsetof(struct cmd_mgmt_tx, buf),
                            frame_len) || frame_len < HOSTED_MGMT_HDR_LEN)
        return hosted_send_mgmt_accept(CMD_RESPONSE_INVALID, seq);
    if (if_type != ESP_AP_IF || !softap_started)
        return hosted_send_mgmt_accept(CMD_RESPONSE_INVALID, seq);

    portENTER_CRITICAL(&s_hosted_mgmt_lock);
    if (s_hosted_mgmt_draining || s_hosted_mgmt.pending) {
        portEXIT_CRITICAL(&s_hosted_mgmt_lock);
        return hosted_send_mgmt_accept(CMD_RESPONSE_BUSY, seq);
    }
    portEXIT_CRITICAL(&s_hosted_mgmt_lock);

    ret = mgmt_tx_ensure_cb();
    if (ret)
        return hosted_send_mgmt_accept(CMD_RESPONSE_BUSY, seq);

    broadcast = IS_BROADCAST_ADDR(mgmt_tx->buf + 4);
    track = !broadcast && !mgmt_tx->dont_wait_for_ack;
    if (track) {
        ret = hosted_mgmt_begin(cookie, mgmt_tx->buf, frame_len, true);
        if (ret)
            return hosted_send_mgmt_accept(CMD_RESPONSE_BUSY, seq);
    }

    ret = ieee80211_send_mgmt_internal(WIFI_IF_AP, mgmt_tx->buf, frame_len);
    if (ret) {
        if (track)
            hosted_mgmt_abort_pending();
        return hosted_send_mgmt_accept(CMD_RESPONSE_BUSY, seq);
    }

    ret = hosted_send_mgmt_accept(CMD_RESPONSE_SUCCESS, seq);
    if (ret != ESP_OK) {
        if (track)
            hosted_mgmt_abort_pending();
        return ret;
    }

    if (track) {
        hosted_mgmt_mark_accepted(cookie);
    } else {
        /* Broadcast/multicast management frames and frames with dont_wait_for_ack
         * are never ACKed at 802.11. Report successful submission with ack=false
         * so the host cfg80211 driver can complete its TX status reporting. */
        (void)hosted_send_mgmt_status(false, mgmt_tx->buf, frame_len, cookie);
    }
    return ESP_OK;
}

int ieee80211_add_node(wifi_interface_t wifi_if_type, uint8_t *mac, uint16_t aid,
                       uint8_t *rates, uint8_t *htcap, uint8_t *vhtcap, uint8_t *hecap);

int add_station_node_ap(void *data)
{
    struct cmd_ap_add_sta_config *sta = (struct cmd_ap_add_sta_config *) data;
    uint16_t cmd = le16toh(sta->sta_param.cmd);
    uint32_t flags_set = le32toh(sta->sta_param.sta_flags_set);
    uint32_t flags_mask = le32toh(sta->sta_param.sta_flags_mask);
    uint16_t aid = le16toh(sta->sta_param.aid);

    ESP_LOG_BUFFER_HEXDUMP("mac", sta->sta_param.mac, 6, ESP_LOG_INFO);
    ESP_LOGI(TAG, "aid=%d\n", aid);

#define STA_FLAG_AUTHORIZED 1
    if (cmd != ADD_STA) {
        int err = 0;
        ESP_LOGD(TAG, "sta_flags_set=%lu, sta_flags_mask=%lu sta_modify_mask=%lu\n",
                 (unsigned long)flags_set, (unsigned long)flags_mask,
                 (unsigned long)le32toh(sta->sta_param.sta_modify_mask));
        if (flags_set & BIT(STA_FLAG_AUTHORIZED)) {
            ESP_LOGI(TAG, "%s: authorizing station\n", __func__);
            err = esp_wifi_wpa_ptk_init_done_internal(sta->sta_param.mac);
        } else if ((flags_mask & BIT(STA_FLAG_AUTHORIZED)) &&
                   !(flags_set & BIT(STA_FLAG_AUTHORIZED))) {
            ESP_LOGE(TAG, "%s: deauthorizing station without deletion is unsupported\n", __func__);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (err) {
            ESP_LOGE(TAG, "%s: station modify failed err=%d\n", __func__, err);
            return err;
        }
        return 0;
    } else {
        ESP_LOG_BUFFER_HEXDUMP("supported_rates", sta->sta_param.supported_rates, 12, ESP_LOG_INFO);
        ESP_LOG_BUFFER_HEXDUMP("ht_rates", sta->sta_param.ht_caps, 28, ESP_LOG_INFO);
        ESP_LOG_BUFFER_HEXDUMP("vht_rates", sta->sta_param.vht_caps, 14, ESP_LOG_INFO);
        ESP_LOG_BUFFER_HEXDUMP("he_rates", sta->sta_param.he_caps, 27, ESP_LOG_INFO);
        return ieee80211_add_node(WIFI_IF_AP, sta->sta_param.mac, aid,
                                  sta->sta_param.supported_rates[0] ? sta->sta_param.supported_rates : NULL,
                                  sta->sta_param.ht_caps[0] ? sta->sta_param.ht_caps : NULL,
                                  sta->sta_param.vht_caps[0] ? sta->sta_param.vht_caps : NULL,
                                  sta->sta_param.he_caps[0] ? sta->sta_param.he_caps : NULL);
    }
}

static int add_node_ap(void *args)
{
    wifi_ipc_config_t cfg;

    cfg.fn = add_station_node_ap;
    cfg.arg = args;
    cfg.arg_size = 0;
    return esp_wifi_ipc_internal(&cfg, true);
}

int process_ap_station(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    uint8_t cmd_status = CMD_RESPONSE_SUCCESS;
    int ret;

    if (if_type != ESP_AP_IF || !payload || payload_len < sizeof(struct cmd_ap_add_sta_config)) {
        ESP_LOGE(TAG, "%s: invalid arguments or wrong interface=%d len=%u\n",
                 __func__, if_type, payload_len);
        cmd_status = CMD_RESPONSE_INVALID;
        goto send_resp;
    }

    ESP_LOGI(TAG, "%s:got station add command\n", __func__);
    ret = add_node_ap(payload);
    if (ret) {
        ESP_LOGE(TAG, "add_node_ap failed ret=%d", ret);
        cmd_status = CMD_RESPONSE_FAIL;
    }
send_resp:
    return send_command_resp(if_type, CMD_AP_STATION, cmd_status, NULL, 0);
}

typedef int (*cmd_handler_fn)(uint8_t if_type, uint8_t *payload, uint16_t payload_len);

struct esp_cmd_entry {
    uint8_t        cmd_code;
    uint16_t       min_payload_len;
    cmd_handler_fn handler;
    const char    *name;
};

static const struct esp_cmd_entry s_cmd_table[] = {
    { CMD_INIT_INTERFACE,      0,                                      process_init_interface,     "INIT_IF" },
    { CMD_SET_MAC,             sizeof(struct cmd_config_mac_address),  process_set_mac,            "SET_MAC" },
    { CMD_GET_MAC,             0,                                      process_get_mac,            "GET_MAC" },
    { CMD_SCAN_REQUEST,        sizeof(struct scan_request),            process_start_scan,         "SCAN_REQ" },
    { CMD_STA_CONNECT,         0,                                      process_sta_connect,        "STA_CONNECT" },
    { CMD_DISCONNECT,          sizeof(struct cmd_disconnect),          process_disconnect,         "DISCONNECT" },
    { CMD_DEINIT_INTERFACE,    0,                                      process_deinit_interface,   "DEINIT_IF" },
    { CMD_ADD_KEY,             0,                                      process_add_key,            "ADD_KEY" },
    { CMD_DEL_KEY,             0,                                      process_del_key,            "DEL_KEY" },
    { CMD_SET_DEFAULT_KEY,     0,                                      process_set_default_key,    "SET_DEF_KEY" },
    { CMD_STA_AUTH,            0,                                      process_auth_request,       "STA_AUTH" },
    { CMD_STA_ASSOC,           0,                                      process_assoc_request,      "STA_ASSOC" },
    { CMD_SET_IP_ADDR,         sizeof(struct cmd_set_ip_addr),         process_set_ip,             "SET_IP" },
    { CMD_SET_MCAST_MAC_ADDR,  sizeof(struct cmd_set_mcast_mac_addr),  process_set_mcast_mac_list, "SET_MCAST" },
    { CMD_GET_TXPOWER,         0,                                      process_get_tx_power,       "GET_TXPOWER" },
    { CMD_SET_TXPOWER,         sizeof(struct cmd_set_get_val),         process_set_tx_power,       "SET_TXPOWER" },
    { CMD_GET_REG_DOMAIN,      0,                                      process_reg_get,            "GET_REG" },
    { CMD_SET_REG_DOMAIN,      sizeof(struct cmd_reg_domain),          process_reg_set,            "SET_REG" },
    { CMD_RAW_TP_ESP_TO_HOST,  0,                                      process_raw_tp,             "RAW_TP_E2H" },
    { CMD_RAW_TP_HOST_TO_ESP,  0,                                      process_raw_tp,             "RAW_TP_H2E" },
    { CMD_SET_WOW_CONFIG,      sizeof(struct cmd_wow_config),          process_wow_set,            "SET_WOW" },
    { CMD_SET_MODE,            0,                                      process_set_mode,           "SET_MODE" },
    { CMD_SET_IE,              0,                                      process_set_ie,             "SET_IE" },
    { CMD_AP_CONFIG,           0,                                      process_set_ap_config,      "AP_CONFIG" },
    { CMD_MGMT_TX,             0,                                      process_mgmt_tx,            "MGMT_TX" },
    { CMD_AP_STATION,          sizeof(struct cmd_ap_add_sta_config),   process_ap_station,         "AP_STATION" },
    { CMD_STA_RSSI,            0,                                      process_rssi,               "STA_RSSI" },
    { CMD_SET_TIME,            sizeof(struct cmd_set_time),            process_set_time,           "SET_TIME" },
    { CMD_START_OTA_UPDATE,    0,                                      process_ota_start,          "OTA_START" },
    { CMD_START_OTA_WRITE,     0,                                      process_ota_write,          "OTA_WRITE" },
    { CMD_START_OTA_END,       0,                                      process_ota_end,            "OTA_END" },
    { CMD_STA_SET_AUTHORIZED,  sizeof(struct cmd_sta_set_authorized), process_sta_set_authorized, "STA_SET_AUTH" },
};

int esp_cmd_dispatch(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    struct command_header *header;
    const struct esp_cmd_entry *entry = NULL;
    size_t i;

    if (!payload || payload_len < sizeof(*header)) {
        ESP_LOGE(TAG, "CMD_RX_INVALID if=%u len=%u expected=%u; rejecting command",
                 if_type, payload_len, (unsigned)sizeof(*header));
        return ESP_ERR_INVALID_ARG;
    }

    header = (struct command_header *)payload;
    active_cmd_code = header->cmd_code;
    active_cmd_seq = le16toh(header->seq_num);

    for (i = 0; i < sizeof(s_cmd_table) / sizeof(s_cmd_table[0]); i++) {
        if (s_cmd_table[i].cmd_code == header->cmd_code) {
            entry = &s_cmd_table[i];
            break;
        }
    }

    if (!entry) {
        ESP_LOGW(TAG, "Unsupported cmd[0x%x] received", header->cmd_code);
        send_command_resp(if_type, header->cmd_code, CMD_RESPONSE_UNSUPPORTED, NULL, 0);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (payload_len < entry->min_payload_len) {
        ESP_LOGE(TAG, "CMD_RX_TRUNCATED handler=%s len=%u expected=%u; rejecting command",
                 entry->name, payload_len, entry->min_payload_len);
        send_command_resp(if_type, header->cmd_code, CMD_RESPONSE_FAIL, NULL, 0);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "CMD_RX: %s code=%u seq=%u len=%u if=%u",
             entry->name, header->cmd_code, active_cmd_seq, payload_len, if_type);

    return entry->handler(if_type, payload, payload_len);
}
