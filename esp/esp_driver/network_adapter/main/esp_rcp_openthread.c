// SPDX-License-Identifier: Apache-2.0
/*
 * ESP-IDF OpenThread RCP binding for the ESP-Hosted byte-stream transport.
 *
 * The Hosted transport carries HDLC-framed Spinel bytes unchanged. OpenThread
 * owns the 802.15.4 radio/NCP state; this file only binds its custom transport
 * callbacks to esp_hosted_rcp_*().
 */
#include "sdkconfig.h"

#if defined(CONFIG_ESP_HOSTED_RCP_OPENTHREAD)

#include <stdbool.h>

#include "esp_err.h"

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"
#endif

#include "esp_check.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_system.h"
#include "esp_openthread_transport.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "openthread/instance.h"
#include "soc/soc_caps.h"

#include "esp_rcp.h"

#if !SOC_IEEE802154_SUPPORTED
#error "ESP-Hosted OpenThread RCP backend requires IEEE 802.15.4 hardware"
#endif

#ifdef CONFIG_OPENTHREAD_TASK_SIZE
#define ESP_HOSTED_RCP_OT_TASK_STACK CONFIG_OPENTHREAD_TASK_SIZE
#else
#define ESP_HOSTED_RCP_OT_TASK_STACK 4096
#endif

#ifdef CONFIG_OPENTHREAD_TASK_PRIORITY
#define ESP_HOSTED_RCP_OT_TASK_PRIO  CONFIG_OPENTHREAD_TASK_PRIORITY
#else
#define ESP_HOSTED_RCP_OT_TASK_PRIO  5
#endif

#define ESP_HOSTED_RCP_OT_NETIF_Q    10
#define ESP_HOSTED_RCP_OT_TASK_Q     10

static const char *TAG = "FW_RCP_OT";
static TaskHandle_t s_ot_task;
static SemaphoreHandle_t s_start_sem;
static esp_err_t s_start_result = ESP_FAIL;
static bool s_eventfd_registered;

extern void otAppNcpInit(otInstance *instance);

static esp_err_t hosted_rcp_transport_tx(const void *buf, size_t len)
{
    return esp_hosted_rcp_send_to_host((const uint8_t *)buf, len);
}

static esp_err_t hosted_rcp_transport_rx(void *buf, size_t buf_size,
                                         size_t *out_len)
{
    size_t len;

    if (!buf || !out_len)
        return ESP_ERR_INVALID_ARG;

    len = esp_hosted_rcp_read((uint8_t *)buf, buf_size);
    *out_len = len;
    return len ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void hosted_rcp_rx_notify(void *arg)
{
    (void)arg;
    esp_openthread_transport_notify_rcp_rx();
}

static void hosted_rcp_ot_task(void *arg)
{
    esp_openthread_platform_config_t config = {
        .radio_config = {
            .radio_mode = RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = HOST_CONNECTION_MODE_RCP_TRANSPORT,
            .host_transport_config = {
                .transport_tx = hosted_rcp_transport_tx,
                .transport_rx = hosted_rcp_transport_rx,
                .bus_speed = 0,
            },
        },
        .port_config = {
            .storage_partition_name = "nvs",
            .netif_queue_size = ESP_HOSTED_RCP_OT_NETIF_Q,
            .task_queue_size = ESP_HOSTED_RCP_OT_TASK_Q,
        },
    };
    esp_err_t ret;
    bool ot_initialized = false;

    (void)arg;

    ret = esp_openthread_init(&config);
    if (ret == ESP_OK) {
        ot_initialized = true;
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
        ret = esp_coex_wifi_i154_enable();
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "Wi-Fi/802.15.4 coexistence enable failed: %s",
                     esp_err_to_name(ret));
#endif
    } else {
        ESP_LOGE(TAG, "esp_openthread_init failed: %s", esp_err_to_name(ret));
    }

    if (ret == ESP_OK) {
        otAppNcpInit(esp_openthread_get_instance());
        ESP_LOGI(TAG, "OpenThread RCP ready over ESP-Hosted");
    }

    if (ret != ESP_OK) {
        /*
         * Finish backend teardown before waking app_main. Otherwise app_main
         * can deinitialize the generic Hosted bridge while OpenThread is still
         * unwinding its custom transport callbacks.
         */
        if (ot_initialized)
            esp_openthread_deinit();
        esp_hosted_rcp_set_rx_notify(NULL, NULL);
        if (s_eventfd_registered) {
            esp_vfs_eventfd_unregister();
            s_eventfd_registered = false;
        }
        s_ot_task = NULL;
        s_start_result = ret;
        if (s_start_sem)
            xSemaphoreGive(s_start_sem);
        vTaskDelete(NULL);
        return;
    }

    s_start_result = ESP_OK;
    if (s_start_sem)
        xSemaphoreGive(s_start_sem);

    {
        esp_err_t mainloop_ret = esp_openthread_launch_mainloop();

        /*
         * There is no normal hot-stop path for the Hosted RCP. Once
         * ESP_EXT_CAP_RCP is advertised, losing the NCP mainloop leaves the
         * host with a live-looking but unusable Spinel stream. Treat any
         * return as an RCP-incarnation failure and reboot after cleanup.
         */
        ESP_LOGE(TAG, "OpenThread RCP mainloop exited unexpectedly: %s",
                 esp_err_to_name(mainloop_ret));
    }

    /*
     * ESP-IDF v6.1 does not support normal RCP deinitialization after the NCP
     * mainloop has run. This path is fatal anyway: stop Hosted notifications
     * and reboot the complete firmware incarnation instead of attempting a
     * hot deinit of the native-radio RCP.
     */
    esp_hosted_rcp_set_rx_notify(NULL, NULL);
    ESP_LOGE(TAG, "Restarting after OpenThread RCP backend exit");
    esp_hosted_rcp_fatal_restart("OpenThread RCP mainloop exited");
    vTaskDelete(NULL);
}

esp_err_t esp_hosted_rcp_openthread_start(void)
{
    esp_vfs_eventfd_config_t eventfd_config = {
        /* OpenThread task queue + native radio + Hosted transport notify. */
        .max_fds = 3,
    };

    if (s_ot_task)
        return ESP_OK;

    if (!s_eventfd_registered) {
        esp_err_t ret = esp_vfs_eventfd_register(&eventfd_config);
        if (ret != ESP_OK)
            return ret;
        s_eventfd_registered = true;
    }

    if (!s_start_sem) {
        s_start_sem = xSemaphoreCreateBinary();
        if (!s_start_sem) {
            if (s_eventfd_registered) {
                esp_vfs_eventfd_unregister();
                s_eventfd_registered = false;
            }
            return ESP_ERR_NO_MEM;
        }
    }

    s_start_result = ESP_FAIL;
    esp_hosted_rcp_set_rx_notify(hosted_rcp_rx_notify, NULL);

    if (xTaskCreate(hosted_rcp_ot_task, "hosted_ot_rcp",
                    ESP_HOSTED_RCP_OT_TASK_STACK, NULL,
                    ESP_HOSTED_RCP_OT_TASK_PRIO, &s_ot_task) != pdTRUE) {
        esp_hosted_rcp_set_rx_notify(NULL, NULL);
        vSemaphoreDelete(s_start_sem);
        s_start_sem = NULL;
        if (s_eventfd_registered) {
            esp_vfs_eventfd_unregister();
            s_eventfd_registered = false;
        }
        return ESP_ERR_NO_MEM;
    }

    /*
     * Do not advertise ESP_EXT_CAP_RCP until the RCP NCP is initialized.
     * Boot is intentionally held here so userspace never observes an endpoint
     * whose firmware backend is not ready yet.
     */
    xSemaphoreTake(s_start_sem, portMAX_DELAY);
    vSemaphoreDelete(s_start_sem);
    s_start_sem = NULL;

    /*
     * On an initialization failure the worker exits immediately. The worker
     * owns eventfd unregister so it cannot race with a partially initialized
     * OpenThread platform teardown.
     */
    return s_start_result;
}

#endif /* CONFIG_ESP_HOSTED_RCP_OPENTHREAD */
