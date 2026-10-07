// SPDX-License-Identifier: Apache-2.0
#ifndef __ESP_THREAD_RCP_UART_H__
#define __ESP_THREAD_RCP_UART_H__

#include "esp_err.h"

#ifdef CONFIG_ESP_THREAD_RCP_UART
esp_err_t esp_thread_rcp_uart_start(void);
#endif

#endif
