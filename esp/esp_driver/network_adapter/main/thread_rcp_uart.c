// SPDX-License-Identifier: Apache-2.0
/*
 * Dedicated OpenThread RCP UART transport.
 *
 * This path intentionally bypasses ESP-Hosted. It lets products keep Wi-Fi on
 * SDIO/SPI/USB while exposing Bluetooth HCI and Thread Spinel on independent
 * UARTs (for example BT on UART1 and Thread on UART2).
 */
#include "sdkconfig.h"

#ifdef CONFIG_ESP_THREAD_RCP_UART

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"

#include "thread_rcp_uart.h"

#if defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_CTRL_HCI_INTERFACE_USE_UART)
#if CONFIG_BT_CTRL_HCI_UART_PORT == CONFIG_ESP_THREAD_RCP_UART_PORT
#error "Bluetooth HCI UART and Thread RCP UART must use different UART ports"
#endif
#endif

static const char *TAG = "FW_THREAD_UART";

esp_err_t esp_thread_rcp_uart_start(void)
{
    esp_vfs_eventfd_config_t eventfd_config = {
        /* OpenThread task queue + native IEEE 802.15.4 radio. */
        .max_fds = 2,
    };
    static esp_openthread_config_t config = {
        .netif_config = {0},
        .platform_config = {
            .radio_config = {
                .radio_mode = RADIO_MODE_NATIVE,
            },
            .host_config = {
                .host_connection_mode = HOST_CONNECTION_MODE_RCP_UART,
                .host_uart_config = {
                    .port = (uart_port_t)CONFIG_ESP_THREAD_RCP_UART_PORT,
                    .uart_config = {
                        .baud_rate = CONFIG_ESP_THREAD_RCP_UART_BAUD,
                        .data_bits = UART_DATA_8_BITS,
                        .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1,
                        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        .rx_flow_ctrl_thresh = 0,
                        .source_clk = UART_SCLK_DEFAULT,
                    },
                    .rx_pin = (gpio_num_t)CONFIG_ESP_THREAD_RCP_UART_RX_PIN,
                    .tx_pin = (gpio_num_t)CONFIG_ESP_THREAD_RCP_UART_TX_PIN,
                },
            },
            .port_config = {
                .storage_partition_name = "nvs",
                .netif_queue_size = 10,
                .task_queue_size = 10,
            },
        },
    };
    esp_err_t ret;

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        return ret;

    ret = esp_vfs_eventfd_register(&eventfd_config);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        return ret;

    ret = esp_openthread_start(&config);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to start OpenThread UART RCP");

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    ret = esp_coex_wifi_i154_enable();
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to enable Wi-Fi/802.15.4 coexistence");
#endif

    ESP_LOGI(TAG,
             "OpenThread RCP ready on UART%d baud=%d tx=%d rx=%d",
             CONFIG_ESP_THREAD_RCP_UART_PORT,
             CONFIG_ESP_THREAD_RCP_UART_BAUD,
             CONFIG_ESP_THREAD_RCP_UART_TX_PIN,
             CONFIG_ESP_THREAD_RCP_UART_RX_PIN);
    return ESP_OK;
}

#endif /* CONFIG_ESP_THREAD_RCP_UART */
