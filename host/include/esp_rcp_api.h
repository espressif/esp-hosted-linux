// SPDX-License-Identifier: GPL-2.0-only
#ifndef __ESP_RCP_API_H__
#define __ESP_RCP_API_H__

#include "esp.h"

int esp_init_rcp(struct esp_adapter *adapter);
void esp_rcp_note_boot_event(struct esp_adapter *adapter, bool active_on_boot);
bool esp_rcp_fail_closed_required(struct esp_adapter *adapter);
void esp_rcp_session_ack(struct esp_adapter *adapter,
			 const uint8_t *data, size_t len);
void esp_deinit_rcp(struct esp_adapter *adapter);
/* Consumes skb in all cases. skb->data must point at RCP bytes. */
void esp_rcp_rx(struct esp_adapter *adapter, struct sk_buff *skb);

#endif
