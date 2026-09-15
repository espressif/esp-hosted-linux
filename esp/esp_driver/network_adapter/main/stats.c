// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2026 Espressif Systems (Shanghai) PTE LTD

#include "stats.h"
#include <unistd.h>
#include "esp_log.h"
#include "esp.h"
#include "slave_bt.h"
#include "cmd.h"
#include "esp_fw_version.h"
#include <string.h>
#include "esp_private/wifi.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static const char TAG[] = "stats";
extern volatile uint8_t datapath;

#if TASK_DEFAULT_PRIO > 0
#define RAW_TP_TASK_PRIO (TASK_DEFAULT_PRIO - 1)
#else
#define RAW_TP_TASK_PRIO TASK_DEFAULT_PRIO
#endif

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/* These functions are only for debugging purpose
 * Please do not enable in production environments
 */
static esp_err_t log_real_time_stats(TickType_t xTicksToWait)
{
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    uint32_t start_run_time, end_run_time;
    esp_err_t ret;

    /*Allocate array to store current task states*/
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    /*Get current task states*/
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    /*Allocate array to store tasks states post delay*/
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    /*Get post delay task states*/
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    /*Calculate total_elapsed_time in units of run time stats clock period.*/
    uint32_t total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    ESP_LOGI(TAG, "| Task | Run Time | Percentage");
    /*Match each task in start_array to those in the end_array*/
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                /*Mark that task have been matched by overwriting their handles*/
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        /*Check if matching task found*/
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * portNUM_PROCESSORS);
            ESP_LOGI(TAG, "| %s | %"PRIu32" | %"PRIu32"%%", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    /*Print unmatched tasks*/
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            ESP_LOGI(TAG, "| %s | Deleted", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            ESP_LOGI(TAG, "| %s | Created", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    /*Common return path*/
    if (start_array) {
        free(start_array);
    }
    if (end_array) {
        free(end_array);
    }
    return ret;
}

static void log_runtime_stats_task(void* pvParameters)
{
    while (1) {
        ESP_LOGI(TAG, "\n\nGetting real time stats over %"PRIu32" ticks", STATS_TICKS);
        if (log_real_time_stats(STATS_TICKS) == ESP_OK) {
            ESP_LOGI(TAG, "Real time stats obtained");
        } else {
            ESP_LOGE(TAG, "Error getting real time stats");
        }
        vTaskDelay(pdMS_TO_TICKS(1000 * 2));
    }
}
#endif

static portMUX_TYPE raw_tp_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t raw_tp_interval;

uint8_t raw_tp_tx_buf[TEST_RAW_TP__BUF_SIZE] = {0};
uint64_t test_raw_tp_rx_len;
static uint32_t raw_tp_tx_seq;
static uint32_t raw_tp_tx_seq_local;
static uint32_t raw_tp_rx_expected_seq;
static uint64_t raw_tp_rx_window;
static uint32_t s_raw_tp_run_id;
static uint64_t raw_tp_rx_total;
static uint64_t raw_tp_rx_lost;
static uint64_t raw_tp_rx_dup;
static uint64_t raw_tp_rx_reorder;
static uint32_t raw_tp_rx_error_logs;
static uint64_t raw_tp_tx_failed_frames;
enum raw_tp_task_state {
    RAW_TP_TASK_STOPPED = 0,
    RAW_TP_TASK_STARTING,
    RAW_TP_TASK_RUNNING,
    RAW_TP_TASK_STOPPING,
};

static TaskHandle_t raw_tp_task_handle;
static volatile enum raw_tp_task_state s_raw_tp_task_state = RAW_TP_TASK_STOPPED;
static volatile bool s_raw_tp_task_should_stop;
static volatile uint32_t s_raw_tp_task_gen;
static volatile uint32_t s_raw_tp_stopped_gen;
static SemaphoreHandle_t s_raw_tp_done_sem;
static esp_timer_handle_t raw_tp_timer;
static volatile uint32_t raw_tp_active_type;
static volatile bool s_raw_tp_timer_enabled;
static volatile bool s_raw_tp_timer_in_callback;

static void raw_tp_reset_stats(void)
{
    portENTER_CRITICAL(&raw_tp_stats_lock);
    test_raw_tp_rx_len = 0;
    raw_tp_tx_seq = 0;
    raw_tp_tx_seq_local = 0;
    raw_tp_rx_expected_seq = 0;
    raw_tp_rx_window = 0;
    raw_tp_rx_total = 0;
    raw_tp_rx_lost = 0;
    raw_tp_rx_dup = 0;
    raw_tp_rx_reorder = 0;
    raw_tp_rx_error_logs = 0;
    raw_tp_tx_failed_frames = 0;
    raw_tp_interval = 0;
    portEXIT_CRITICAL(&raw_tp_stats_lock);
}

void debug_raw_tp_set_seq(struct esp_payload_header *header, uint32_t seq)
{
    if (!header) {
        return;
    }

    /* These four fields are available for ESP_TEST_IF packets. */
    header->flags = seq & 0xff;
    header->reserved1 = (seq >> 8) & 0xff;
    header->reserved2 = (seq >> 16) & 0xff;
    header->reserved3 = (seq >> 24) & 0xff;
}

uint32_t debug_raw_tp_get_seq(const struct esp_payload_header *header)
{
    if (!header) {
        return 0;
    }

    return (uint32_t)header->flags |
           ((uint32_t)header->reserved1 << 8) |
           ((uint32_t)header->reserved2 << 16) |
           ((uint32_t)header->reserved3 << 24);
}

uint32_t debug_raw_tp_tx_seq_get(void)
{
    uint32_t seq;

    portENTER_CRITICAL(&raw_tp_stats_lock);
    seq = raw_tp_tx_seq;
    portEXIT_CRITICAL(&raw_tp_stats_lock);
    return seq;
}

void debug_raw_tp_tx_failed(uint32_t frame_count)
{
    portENTER_CRITICAL(&raw_tp_stats_lock);
    raw_tp_tx_failed_frames += frame_count;
    portEXIT_CRITICAL(&raw_tp_stats_lock);
}

void debug_raw_tp_tx_complete(uint32_t frame_count)
{
    portENTER_CRITICAL(&raw_tp_stats_lock);
    raw_tp_tx_seq += frame_count;
    test_raw_tp_rx_len += (uint64_t)frame_count * TEST_RAW_TP__BUF_SIZE;
    portEXIT_CRITICAL(&raw_tp_stats_lock);
}

uint32_t debug_raw_tp_get_run_id(void)
{
    return s_raw_tp_run_id;
}

bool debug_raw_tp_is_e2h_active(void)
{
    return raw_tp_active_type == CMD_RAW_TP_ESP_TO_HOST;
}

bool debug_raw_tp_is_run_active(uint32_t run_id)
{
    return (raw_tp_active_type == CMD_RAW_TP_ESP_TO_HOST) &&
           (run_id == s_raw_tp_run_id);
}

void debug_update_raw_tp_rx_count(const struct esp_payload_header *header,
                                  uint16_t len)
{
    uint32_t seq;
    uint32_t run_id = 0;
    int32_t diff;
    uint16_t offset;
    const struct raw_tp_packet *tp_pkt;

    if (raw_tp_active_type != CMD_RAW_TP_HOST_TO_ESP || !header)
        return;

    offset = le16toh(header->offset);
    if (len >= offset + sizeof(struct raw_tp_packet)) {
        tp_pkt = (const struct raw_tp_packet *)((const uint8_t *)header + offset);
        seq = le32toh(tp_pkt->seq);
        run_id = le32toh(tp_pkt->run_id);
    } else {
        seq = debug_raw_tp_get_seq(header);
        run_id = s_raw_tp_run_id;
    }

    if (run_id != s_raw_tp_run_id)
        return;

    portENTER_CRITICAL(&raw_tp_stats_lock);
    diff = (int32_t)(seq - raw_tp_rx_expected_seq);
    raw_tp_rx_total++;
    test_raw_tp_rx_len += len;

    if (!diff) {
        raw_tp_rx_expected_seq++;
        while (raw_tp_rx_window & 1ULL) {
            raw_tp_rx_expected_seq++;
            raw_tp_rx_window >>= 1;
        }
        raw_tp_rx_window >>= 1;
    } else if (diff > 0) {
        if (diff > 128) {
            raw_tp_rx_lost++;
            raw_tp_rx_expected_seq++;
            for (uint32_t i = 0; i < 63; i++) {
                if (!(raw_tp_rx_window & 1ULL))
                    raw_tp_rx_lost++;
                raw_tp_rx_expected_seq++;
                raw_tp_rx_window >>= 1;
            }
            raw_tp_rx_window = 0;
            uint32_t skip = (seq - raw_tp_rx_expected_seq) - 63;
            if (skip > 0) {
                raw_tp_rx_lost += skip;
                raw_tp_rx_expected_seq += skip;
            }
        }
        while ((int32_t)(seq - raw_tp_rx_expected_seq) >= 64) {
            raw_tp_rx_lost++;
            raw_tp_rx_expected_seq++;
            while (raw_tp_rx_window & 1ULL) {
                raw_tp_rx_expected_seq++;
                raw_tp_rx_window >>= 1;
            }
            raw_tp_rx_window >>= 1;
        }
        diff = (int32_t)(seq - raw_tp_rx_expected_seq);
        if (!diff) {
            raw_tp_rx_expected_seq++;
            while (raw_tp_rx_window & 1ULL) {
                raw_tp_rx_expected_seq++;
                raw_tp_rx_window >>= 1;
            }
            raw_tp_rx_window >>= 1;
        } else {
            if (raw_tp_rx_window & (1ULL << (diff - 1))) {
                raw_tp_rx_dup++;
            } else {
                raw_tp_rx_window |= (1ULL << (diff - 1));
                raw_tp_rx_reorder++;
            }
        }
    } else {
        raw_tp_rx_dup++;
    }
    portEXIT_CRITICAL(&raw_tp_stats_lock);
}

static void raw_tp_timer_func(void *arg)
{
    uint64_t bytes;
    uint64_t failed;
    uint64_t total;
    uint64_t lost;
    uint64_t dup;
    uint64_t reorder;
    uint32_t tx_seq;
    uint32_t interval;
    double actual_bandwidth;

    (void)arg;
    portENTER_CRITICAL(&raw_tp_stats_lock);
    if (!s_raw_tp_timer_enabled) {
        portEXIT_CRITICAL(&raw_tp_stats_lock);
        return;
    }
    s_raw_tp_timer_in_callback = true;
    bytes = test_raw_tp_rx_len;
    test_raw_tp_rx_len = 0;
    failed = raw_tp_tx_failed_frames;
    total = raw_tp_rx_total;
    lost = raw_tp_rx_lost;
    dup = raw_tp_rx_dup;
    reorder = raw_tp_rx_reorder;
    tx_seq = raw_tp_tx_seq;
    interval = raw_tp_interval++;
    portEXIT_CRITICAL(&raw_tp_stats_lock);

    actual_bandwidth = (double)(bytes * 8ULL) / 1024.0;
    ESP_LOGI(TAG, "%"PRIu32"-%"PRIu32" sec       %.2f kbits/sec",
             interval, interval + 1, actual_bandwidth);
    ESP_LOGD(TAG, "%"PRIu32"-%"PRIu32" sec %.2f kbits/sec tx_completed=%"PRIu32
             " tx_failed=%"PRIu64" seq_total=%"PRIu64
             " lost=%"PRIu64" dup=%"PRIu64" reorder=%"PRIu64,
             interval, interval + 1, actual_bandwidth, tx_seq, failed,
             total, lost, dup, reorder);

    portENTER_CRITICAL(&raw_tp_stats_lock);
    s_raw_tp_timer_in_callback = false;
    portEXIT_CRITICAL(&raw_tp_stats_lock);
}

static void raw_tp_tx_task(void *pvParameters)
{
    uint32_t my_gen = (uint32_t)(uintptr_t)pvParameters;

    portENTER_CRITICAL(&raw_tp_stats_lock);
    if (s_raw_tp_task_should_stop || s_raw_tp_task_state == RAW_TP_TASK_STOPPING) {
        s_raw_tp_stopped_gen = my_gen;
        s_raw_tp_task_state = RAW_TP_TASK_STOPPED;
        raw_tp_task_handle = NULL;
        portEXIT_CRITICAL(&raw_tp_stats_lock);
        if (s_raw_tp_done_sem)
            xSemaphoreGive(s_raw_tp_done_sem);
        vTaskDelete(NULL);
        return;
    }
    s_raw_tp_task_state = RAW_TP_TASK_RUNNING;
    portEXIT_CRITICAL(&raw_tp_stats_lock);

    while (!s_raw_tp_task_should_stop) {
        if (!datapath || raw_tp_active_type != CMD_RAW_TP_ESP_TO_HOST) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint8_t *tx_buf = heap_caps_malloc(TEST_RAW_TP__BUF_SIZE, MALLOC_CAP_DMA);
        if (!tx_buf) {
            vTaskDelay(1);
            continue;
        }
        memset(tx_buf, 0, TEST_RAW_TP__BUF_SIZE);

        struct raw_tp_packet *tp_pkt = (struct raw_tp_packet *)tx_buf;
        uint32_t cur_seq = raw_tp_tx_seq_local++;
        tp_pkt->seq = htole32(cur_seq);
        tp_pkt->run_id = htole32(s_raw_tp_run_id);

        interface_buffer_handle_t buf_handle = {0};
        buf_handle.if_type = ESP_TEST_IF;
        buf_handle.if_num = 0;
        buf_handle.payload = tx_buf;
        buf_handle.priv_buffer_handle = tx_buf;
        buf_handle.free_buf_handle = heap_caps_free;
        buf_handle.payload_len = TEST_RAW_TP__BUF_SIZE;
        buf_handle.pkt_type = PACKET_TYPE_DATA;
        buf_handle.raw_tp_seq = cur_seq;
        buf_handle.raw_tp_run_id = s_raw_tp_run_id;

        /* Bounded queue wait with cancellation check */
        esp_err_t q_ret = ESP_FAIL;
        while (!s_raw_tp_task_should_stop) {
            q_ret = send_to_host_timeout(PRIO_Q_LOW, &buf_handle, pdMS_TO_TICKS(20));
            if (q_ret == pdTRUE)
                break;
        }

        if (q_ret != pdTRUE) {
            heap_caps_free(tx_buf);
            break;
        }
    }

    portENTER_CRITICAL(&raw_tp_stats_lock);
    s_raw_tp_stopped_gen = my_gen;
    s_raw_tp_task_state = RAW_TP_TASK_STOPPED;
    raw_tp_task_handle = NULL;
    portEXIT_CRITICAL(&raw_tp_stats_lock);

    if (s_raw_tp_done_sem)
        xSemaphoreGive(s_raw_tp_done_sem);

    vTaskDelete(NULL);
}

static void raw_tp_stop_task(void)
{
    uint32_t target_gen;

    portENTER_CRITICAL(&raw_tp_stats_lock);
    if (s_raw_tp_task_state == RAW_TP_TASK_STOPPED && !raw_tp_task_handle) {
        portEXIT_CRITICAL(&raw_tp_stats_lock);
        return;
    }
    target_gen = s_raw_tp_task_gen;
    s_raw_tp_task_should_stop = true;
    s_raw_tp_task_state = RAW_TP_TASK_STOPPING;
    portEXIT_CRITICAL(&raw_tp_stats_lock);

    while (1) {
        portENTER_CRITICAL(&raw_tp_stats_lock);
        bool stopped = (s_raw_tp_task_state == RAW_TP_TASK_STOPPED &&
                        s_raw_tp_stopped_gen == target_gen);
        portEXIT_CRITICAL(&raw_tp_stats_lock);
        if (stopped)
            break;

        if (s_raw_tp_done_sem) {
            (void)xSemaphoreTake(s_raw_tp_done_sem, pdMS_TO_TICKS(20));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    if (s_raw_tp_done_sem)
        xSemaphoreTake(s_raw_tp_done_sem, 0);

    portENTER_CRITICAL(&raw_tp_stats_lock);
    raw_tp_task_handle = NULL;
    s_raw_tp_task_should_stop = false;
    portEXIT_CRITICAL(&raw_tp_stats_lock);
}

static void raw_tp_stop_timer(void)
{
    esp_timer_handle_t timer = raw_tp_timer;
    esp_err_t err = ESP_OK;

    if (!timer)
        return;

    portENTER_CRITICAL(&raw_tp_stats_lock);
    s_raw_tp_timer_enabled = false;
    portEXIT_CRITICAL(&raw_tp_stats_lock);

    if (esp_timer_is_active(timer))
        (void)esp_timer_stop(timer);

    while (s_raw_tp_timer_in_callback)
        vTaskDelay(pdMS_TO_TICKS(1));

    for (int retry = 0; retry < 50; retry++) {
        err = esp_timer_delete(timer);
        if (err == ESP_OK) {
            raw_tp_timer = NULL;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete raw_tp_timer: %s", esp_err_to_name(err));
    }
}

static void raw_tp_stop_resources(bool stop_task)
{
    raw_tp_active_type = 0;
    raw_tp_stop_timer();
    if (stop_task)
        raw_tp_stop_task();
}

void debug_raw_tp_cleanup(void)
{
    raw_tp_stop_resources(true);
    raw_tp_reset_stats();
}

static esp_err_t start_timer_to_display_raw_tp(void)
{
    esp_timer_create_args_t create_args = {
        .callback = &raw_tp_timer_func,
        .arg = NULL,
        .name = "raw_tp_timer",
    };
    esp_err_t ret;

    if (!raw_tp_timer) {
        ret = esp_timer_create(&create_args, &raw_tp_timer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "RAW_TP timer create failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    portENTER_CRITICAL(&raw_tp_stats_lock);
    s_raw_tp_timer_enabled = true;
    portEXIT_CRITICAL(&raw_tp_stats_lock);

    if (!esp_timer_is_active(raw_tp_timer)) {
        ret = esp_timer_start_periodic(raw_tp_timer, TEST_RAW_TP__TIMEOUT);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "RAW_TP timer start failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_OK;
}

static esp_err_t init_raw_tp_timer(void)
{
    return start_timer_to_display_raw_tp();
}

static esp_err_t init_raw_tp_test_task(void)
{
    while (s_raw_tp_task_state == RAW_TP_TASK_STOPPING) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    /* Keep the synthetic producer below send_task. Create it once: a repeated
     * RAW_TP command (for example after a host command retry) must not leak a
     * second infinite producer task. */
    if (s_raw_tp_task_state == RAW_TP_TASK_STARTING ||
        s_raw_tp_task_state == RAW_TP_TASK_RUNNING ||
        raw_tp_task_handle)
        return ESP_OK;

    if (!s_raw_tp_done_sem) {
        s_raw_tp_done_sem = xSemaphoreCreateBinary();
        if (!s_raw_tp_done_sem)
            return ESP_ERR_NO_MEM;
    } else {
        xSemaphoreTake(s_raw_tp_done_sem, 0);
    }

    portENTER_CRITICAL(&raw_tp_stats_lock);
    s_raw_tp_task_should_stop = false;
    s_raw_tp_task_state = RAW_TP_TASK_STARTING;
    uint32_t task_gen = ++s_raw_tp_task_gen;
    portEXIT_CRITICAL(&raw_tp_stats_lock);

    if (xTaskCreate(raw_tp_tx_task, "raw_tp_tx_task",
                    TASK_DEFAULT_STACK_SIZE, (void *)(uintptr_t)task_gen,
                    RAW_TP_TASK_PRIO, &raw_tp_task_handle) != pdTRUE) {
        ESP_LOGE(TAG, "RAW_TP producer task creation failed");
        portENTER_CRITICAL(&raw_tp_stats_lock);
        raw_tp_task_handle = NULL;
        s_raw_tp_task_state = RAW_TP_TASK_STOPPED;
        s_raw_tp_stopped_gen = task_gen;
        portEXIT_CRITICAL(&raw_tp_stats_lock);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void create_debugging_tasks(void)
{
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    if (xTaskCreate(log_runtime_stats_task, "log_runtime_stats_task",
                    TASK_DEFAULT_STACK_SIZE, NULL, TASK_DEFAULT_PRIO,
                    NULL) != pdTRUE)
        ESP_LOGE(TAG, "Runtime stats task creation failed");
#endif
}

void debug_get_raw_tp_conf(uint32_t raw_tp_type)
{
    if (raw_tp_type == CMD_RAW_TP_ESP_TO_HOST) {
        ESP_LOGI(TAG, "\n\n*** Raw Throughput testing: ESP --> Host started ***\n");
    } else if (raw_tp_type == CMD_RAW_TP_HOST_TO_ESP) {
        ESP_LOGI(TAG, "\n\n*** Raw Throughput testing: Host --> ESP started ***\n");
    }
}

void debug_set_wifi_logging(void)
{
    /* set WiFi log level and module */
    uint32_t wifi_log_level = WIFI_LOG_INFO;
#if CONFIG_LOG_MAXIMUM_LEVEL == 0
    wifi_log_level = WIFI_LOG_NONE;
#elif CONFIG_LOG_MAXIMUM_LEVEL == 1
    wifi_log_level = WIFI_LOG_ERROR;
#elif CONFIG_LOG_MAXIMUM_LEVEL == 2
    wifi_log_level = WIFI_LOG_WARNING;
#elif CONFIG_LOG_MAXIMUM_LEVEL == 3
    wifi_log_level = WIFI_LOG_INFO;
#elif CONFIG_LOG_MAXIMUM_LEVEL == 4
    wifi_log_level = WIFI_LOG_DEBUG;
#elif CONFIG_LOG_MAXIMUM_LEVEL == 5
    wifi_log_level = WIFI_LOG_VERBOSE;
#endif
    esp_wifi_internal_set_log_level(wifi_log_level);
}

void debug_log_firmware_version(void)
{
    ESP_LOGI(TAG, "*********************************************************************");
    ESP_LOGI(TAG, "                ESP-Hosted Firmware version :: %s-%d.%d.%d.%d.%d                        ",
             PROJECT_NAME, PROJECT_VERSION_MAJOR_1, PROJECT_VERSION_MAJOR_2, PROJECT_VERSION_MINOR, PROJECT_REVISION_PATCH_1, PROJECT_REVISION_PATCH_2);

#if CONFIG_ESP_SPI_HOST_INTERFACE
#if BLUETOOTH_UART
    ESP_LOGI(TAG, "                Transport used :: SPI + UART                    ");
#else
    ESP_LOGI(TAG, "                Transport used :: SPI only                      ");
#endif
#else
#if BLUETOOTH_UART
    ESP_LOGI(TAG, "                Transport used :: SDIO + UART                   ");
#else
    ESP_LOGI(TAG, "                Transport used :: SDIO only                     ");
#endif
#endif
    ESP_LOGI(TAG, "*********************************************************************");
}

int process_raw_tp(uint8_t if_type, uint8_t *payload, uint16_t payload_len)
{
    interface_buffer_handle_t buf_handle = {0};
    struct command_header *header;
    struct command_header *resp_header;
    esp_err_t setup_ret = ESP_OK;
    int ret;
    bool need_tx_task;

    if (!payload || payload_len < sizeof(struct command_header)) {
        ESP_LOGE(TAG, "Invalid raw throughput command len=%u", payload_len);
        return ESP_ERR_INVALID_ARG;
    }

    header = (struct command_header *)payload;
    if (header->cmd_code != CMD_RAW_TP_ESP_TO_HOST &&
        header->cmd_code != CMD_RAW_TP_HOST_TO_ESP) {
        ESP_LOGE(TAG, "Invalid raw throughput command code=%u", header->cmd_code);
        return ESP_ERR_INVALID_ARG;
    }
    need_tx_task = header->cmd_code == CMD_RAW_TP_ESP_TO_HOST;

    buf_handle.if_type = if_type;
    buf_handle.if_num = 0;
    buf_handle.payload_len = sizeof(struct command_header);
    buf_handle.pkt_type = PACKET_TYPE_COMMAND_RESPONSE;
    buf_handle.payload = heap_caps_malloc(buf_handle.payload_len, MALLOC_CAP_DMA);
    if (!buf_handle.payload)
        return ESP_ERR_NO_MEM;
    memset(buf_handle.payload, 0, buf_handle.payload_len);
    resp_header = (struct command_header *)buf_handle.payload;

    /* Quiesce publication while the next run is prepared. H2E needs no
     * firmware producer, so tear down any E2H task immediately on a direction
     * switch. Restart the timer so each run begins on a fresh one-second
     * measurement boundary. */
    raw_tp_active_type = 0;
    if (!need_tx_task)
        raw_tp_stop_task();
    raw_tp_stop_timer();

    setup_ret = init_raw_tp_timer();
    if (setup_ret == ESP_OK && need_tx_task)
        setup_ret = init_raw_tp_test_task();

    resp_header->cmd_code = header->cmd_code;
    resp_header->len = 0;
    resp_header->cmd_status = setup_ret == ESP_OK ?
                              CMD_RESPONSE_SUCCESS : CMD_RESPONSE_FAIL;
    resp_header->seq_num = header->seq_num;
    buf_handle.priv_buffer_handle = buf_handle.payload;
    buf_handle.free_buf_handle = free;

    if (setup_ret == ESP_OK) {
        if (payload_len >= sizeof(struct cmd_raw_tp)) {
            struct cmd_raw_tp *raw_cmd = (struct cmd_raw_tp *)payload;
            s_raw_tp_run_id = le32toh(raw_cmd->run_id);
        } else {
            s_raw_tp_run_id++;
        }
        if (!s_raw_tp_run_id)
            s_raw_tp_run_id = 1;
        raw_tp_reset_stats();
    }

    ret = send_command_response(&buf_handle);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Slave -> Host: Failed to send RAW_TP response");
        if (buf_handle.payload)
            free(buf_handle.payload);
        raw_tp_stop_resources(true);
        return ret;
    }

    if (setup_ret != ESP_OK) {
        ESP_LOGE(TAG, "RAW_TP setup failed ret=%s", esp_err_to_name(setup_ret));
        raw_tp_stop_resources(true);
        return setup_ret;
    }

    raw_tp_active_type = header->cmd_code;
    debug_get_raw_tp_conf(header->cmd_code);
    return ESP_OK;
}
