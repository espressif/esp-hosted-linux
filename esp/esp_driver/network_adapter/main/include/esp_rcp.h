// SPDX-License-Identifier: Apache-2.0
#ifndef __ESP_HOSTED_RCP_H__
#define __ESP_HOSTED_RCP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*esp_hosted_rcp_rx_notify_cb_t)(void *arg);

esp_err_t esp_hosted_rcp_init(void);
void esp_hosted_rcp_deinit(void);
esp_err_t esp_hosted_rcp_rx_from_host(const uint8_t *data, size_t len);
esp_err_t esp_hosted_rcp_session_start_from_host(uint8_t version,
                                                 uint64_t nonce);
uint64_t esp_hosted_rcp_session_nonce(void);
bool esp_hosted_rcp_fail_closed_required(void);
size_t esp_hosted_rcp_read(uint8_t *buf, size_t buf_size);
void esp_hosted_rcp_set_rx_notify(esp_hosted_rcp_rx_notify_cb_t cb, void *arg);
esp_err_t esp_hosted_rcp_send_to_host(const uint8_t *data, size_t len);
esp_err_t esp_hosted_rcp_host_ready(void);
void esp_hosted_rcp_fatal_restart(const char *reason);

#ifdef CONFIG_ESP_HOSTED_RCP_OPENTHREAD
esp_err_t esp_hosted_rcp_openthread_start(void);
#endif

#endif
