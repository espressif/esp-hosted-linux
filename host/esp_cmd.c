// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 */
#include "utils.h"
#include "esp_cmd.h"
#include "esp_api.h"
#include "esp_utils.h"
#include "esp.h"
#include "esp_if.h"
#include "esp_cfg80211.h"
#include "esp_kernel_port.h"
#include "esp_stats.h"
#include <linux/wait.h>
#include <linux/skbuff.h>
#include <linux/stddef.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/mutex.h>

#define COMMAND_RESPONSE_TIMEOUT (5 * HZ)
#define SCAN_COMPLETION_TIMEOUT  (30 * HZ)
#ifndef U8_MAX
#define U8_MAX			((u8)~0U)
#endif
#ifndef U16_MAX
#define U16_MAX			((u16)~0U)
#endif

/* Keep every pre-existing command/event structure byte-for-byte compatible.
 * A new host opts into split MGMT-TX completion through an existing reserved
 * command-header byte. Event code 7 is an additive event that old hosts never
 * receive because unmarked requests stay on the legacy firmware path. */
#define HOSTED_MGMT_TX_ASYNC_STATUS_V1 0xA5
#define HOSTED_EVENT_MGMT_TX_STATUS    7

struct hosted_mgmt_tx_status_event {
	struct event_header header;
	uint64_t cookie;
	uint32_t frame_len;
	uint8_t ack;
	uint8_t pad[3];
	uint8_t frame[];
} __packed;
extern u32 raw_tp_mode;

static inline bool esp_size_add_overflow(size_t a, size_t b, size_t *sum)
{
	if (a > SIZE_MAX - b)
		return true;
	*sum = a + b;
	return false;
}

void esp_wifi_put_bss(struct esp_wifi_device *priv)
{
	struct cfg80211_bss *bss;
	struct wiphy *wiphy;

	if (!priv)
		return;

	spin_lock_bh(&priv->bss_lock);
	bss = priv->bss;
	priv->bss = NULL;
	wiphy = (priv->adapter) ? priv->adapter->wiphy : NULL;
	spin_unlock_bh(&priv->bss_lock);

	if (bss && wiphy)
		cfg80211_put_bss(wiphy, bss);
}

static void esp_wifi_ref_bss(struct esp_wifi_device *priv, struct cfg80211_bss *bss)
{
	struct cfg80211_bss *old;
	struct wiphy *wiphy;

	if (!priv || !bss || !priv->adapter || !priv->adapter->wiphy)
		return;

	wiphy = priv->adapter->wiphy;
	cfg80211_ref_bss(wiphy, bss);

	spin_lock_bh(&priv->bss_lock);
	old = priv->bss;
	if (old == bss) {
		spin_unlock_bh(&priv->bss_lock);
		cfg80211_put_bss(wiphy, bss);
		return;
	}
	priv->bss = bss;
	spin_unlock_bh(&priv->bss_lock);

	if (old)
		cfg80211_put_bss(wiphy, old);
}

static void esp_wifi_take_assoc_bss(struct esp_wifi_device *priv,
		struct cfg80211_bss *bss)
{
	struct cfg80211_bss *old;
	struct wiphy *wiphy;
	bool put_extra = false;

	if (!priv || !bss || !priv->adapter || !priv->adapter->wiphy)
		return;

	wiphy = priv->adapter->wiphy;
	spin_lock_bh(&priv->bss_lock);
	old = priv->bss;
	if (!old) {
		/* Reassoc without a new .auth has no stored BSS. Keep the
		 * reference cfg80211 transferred if the assoc event is still
		 * outstanding. If the event already consumed the auth BSS,
		 * drop the extra .assoc reference. */
		if (priv->assoc_cmd_pending || priv->assoc_awaiting_mlme)
			priv->bss = bss;
		else
			put_extra = true;
		spin_unlock_bh(&priv->bss_lock);
		if (put_extra)
			cfg80211_put_bss(wiphy, bss);
		return;
	}
	if (old == bss) {
		spin_unlock_bh(&priv->bss_lock);
		cfg80211_put_bss(wiphy, bss);
		return;
	}
	priv->bss = bss;
	spin_unlock_bh(&priv->bss_lock);
	cfg80211_put_bss(wiphy, old);
}

static bool skb_has_flex_frame(const struct sk_buff *skb, size_t prefix, u32 frame_len);

static void esp_wifi_clear_auth_pending(struct esp_wifi_device *priv)
{
	if (!priv)
		return;

	spin_lock_bh(&priv->bss_lock);
	priv->auth_cmd_pending = false;
	priv->auth_cmd_seq = 0;
	priv->auth_awaiting_mlme = false;
	memset(priv->auth_bssid, 0, MAC_ADDR_LEN);
	spin_unlock_bh(&priv->bss_lock);
}

static void esp_wifi_clear_assoc_pending(struct esp_wifi_device *priv)
{
	u8 *ie;
	struct sk_buff *staged;

	if (!priv)
		return;

	spin_lock_bh(&priv->bss_lock);
	priv->assoc_cmd_pending = false;
	priv->assoc_cmd_seq = 0;
	priv->assoc_control_port = false;
	priv->assoc_awaiting_mlme = false;
	priv->assoc_mlme_notified = false;
	priv->staged_assoc_seq = 0;
	memset(priv->assoc_bssid, 0, MAC_ADDR_LEN);
	ie = priv->assoc_req_ie;
	priv->assoc_req_ie = NULL;
	priv->assoc_req_ie_len = 0;
	staged = priv->staged_assoc_skb;
	priv->staged_assoc_skb = NULL;
	spin_unlock_bh(&priv->bss_lock);
	kfree(ie);
	kfree_skb(staged);
}

enum esp_mlme_op {
	ESP_MLME_AUTH,
	ESP_MLME_ASSOC,
	ESP_MLME_DISCONNECT,
};

static void esp_deliver_auth_from_skb(struct esp_wifi_device *priv,
				      struct sk_buff *skb)
{
	struct auth_event *event;
	u16 frame_len;

	if (!priv || !skb || !priv->ndev ||
	    skb->len < offsetof(struct auth_event, frame))
		return;

	event = (struct auth_event *)skb->data;
	frame_len = esp_wire_le16_to_cpu(event->frame_len);
	if (!skb_has_flex_frame(skb, offsetof(struct auth_event, frame), frame_len))
		return;

	spin_lock_bh(&priv->bss_lock);
	if (!priv->auth_awaiting_mlme ||
	    !priv->auth_cmd_seq ||
	    esp_wire_le16_to_cpu(event->auth_seq) != priv->auth_cmd_seq ||
	    memcmp(priv->auth_bssid, event->bssid, MAC_ADDR_LEN)) {
		spin_unlock_bh(&priv->bss_lock);
		return;
	}
	priv->auth_awaiting_mlme = false;
	priv->auth_cmd_seq = 0;
	memset(priv->auth_bssid, 0, MAC_ADDR_LEN);
	spin_unlock_bh(&priv->bss_lock);

	esp_hex_dump_verbose("Auth frame: ", event->frame, frame_len);
	cfg80211_rx_mlme_mgmt(priv->ndev, event->frame, frame_len);
}

static void esp_deliver_assoc_from_skb(struct esp_wifi_device *priv,
				       struct sk_buff *skb)
{
	struct assoc_event *event;
	struct cfg80211_bss *bss;
	u8 *assoc_ie;
	size_t assoc_ie_len;
	u16 frame_len;
	u16 status_code = 0;

	if (!priv || !skb || !priv->ndev ||
	    skb->len < offsetof(struct assoc_event, frame))
		return;

	event = (struct assoc_event *)skb->data;
	frame_len = esp_wire_le16_to_cpu(event->frame_len);
	if (!skb_has_flex_frame(skb, offsetof(struct assoc_event, frame), frame_len))
		return;

	if (frame_len >= IEEE_HEADER_SIZE + 4)
		status_code = esp_wire_le16_to_cpu(*(u16 *)(event->frame + IEEE_HEADER_SIZE + 2));

	spin_lock_bh(&priv->bss_lock);
	if ((!priv->assoc_cmd_pending && !priv->assoc_awaiting_mlme) ||
	    !priv->assoc_cmd_seq ||
	    esp_wire_le16_to_cpu(event->assoc_seq) != priv->assoc_cmd_seq ||
	    memcmp(priv->assoc_bssid, event->bssid, MAC_ADDR_LEN)) {
		spin_unlock_bh(&priv->bss_lock);
		esp_info("Drop delivery of stale staged ASSOC (event_seq=%u cmd_seq=%u)\n",
			 esp_wire_le16_to_cpu(event->assoc_seq), priv->assoc_cmd_seq);
		return;
	}

	priv->assoc_cmd_pending = false;
	priv->assoc_awaiting_mlme = false;
	priv->assoc_mlme_notified = true;
	if (status_code == 0) {
		priv->conn_generation = priv->assoc_cmd_seq;
		priv->stop_data = 0;
		priv->port_open = priv->assoc_control_port ? 0 : 1;
	} else if (!wireless_dev_current_bss_exists(&priv->wdev)) {
		priv->conn_generation = 0;
		priv->stop_data = 1;
		priv->port_open = 0;
	}
	priv->staged_assoc_seq = 0;
	priv->assoc_cmd_seq = 0;
	priv->assoc_control_port = false;
	memset(priv->assoc_bssid, 0, MAC_ADDR_LEN);
	bss = priv->bss;
	priv->bss = NULL;
	assoc_ie = priv->assoc_req_ie;
	priv->assoc_req_ie = NULL;
	assoc_ie_len = priv->assoc_req_ie_len;
	priv->assoc_req_ie_len = 0;
	priv->rssi = event->rssi;
	spin_unlock_bh(&priv->bss_lock);

	CFG80211_RX_ASSOC_RESP(priv->ndev, bss, event->frame, frame_len,
			0, assoc_ie, assoc_ie_len);
	kfree(assoc_ie);
}

#define IEEE80211_DEAUTH_FRAME_LEN      (24 /* hdr */ + 2 /* reason */)

static void process_deauth_event(struct esp_wifi_device *priv,
				 struct disconnect_event *event)
{
	u8 frame_buf[IEEE80211_DEAUTH_FRAME_LEN];
	struct ieee80211_mgmt *mgmt = (void *)frame_buf;
	u16 stype = IEEE80211_STYPE_DEAUTH;

	if (!priv || !priv->ndev || !event)
		return;

	if (event->header.status == DISCONNECT_TYPE_DISASSOC)
		stype = IEEE80211_STYPE_DISASSOC;

	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | stype);
	mgmt->duration = 0;
	mgmt->seq_ctrl = 0;
	memcpy(mgmt->da, priv->mac_address, ETH_ALEN);
	memcpy(mgmt->sa, event->bssid, ETH_ALEN);
	memcpy(mgmt->bssid, event->bssid, ETH_ALEN);
	mgmt->u.deauth.reason_code = cpu_to_le16(event->reason);

	cfg80211_rx_mlme_mgmt(priv->ndev, frame_buf, IEEE80211_DEAUTH_FRAME_LEN);
}

void esp_deliver_disconnect_from_skb(struct esp_wifi_device *priv,
				    struct sk_buff *skb)
{
	struct disconnect_event *event;
	char ssid[MAX_SSID_LEN + 1];
	u16 disc_seq = 0;
	bool match = false;
	bool local = false;
	struct sk_buff *staged_assoc = NULL;

	if (!priv || !skb || skb->len < sizeof(*event))
		return;

	event = (struct disconnect_event *)skb->data;
	disc_seq = esp_wire_le16_to_cpu(event->disconnect_seq);

	spin_lock_bh(&priv->bss_lock);
	if ((priv->disconnect_cmd_pending || priv->disconnect_awaiting_mlme) &&
	    priv->disconnect_cmd_seq && disc_seq == priv->disconnect_cmd_seq) {
		match = true;
		local = true;
	} else if ((priv->conn_generation && disc_seq == priv->conn_generation) ||
		   (priv->staged_assoc_seq && disc_seq == priv->staged_assoc_seq)) {
		match = true;
		local = false;
	} else if (priv->local_disconnect_req &&
		   (priv->disconnect_cmd_pending || priv->disconnect_awaiting_mlme) &&
		   !priv->disconnect_cmd_seq) {
		match = true;
		local = true;
	}

	if (!match) {
		spin_unlock_bh(&priv->bss_lock);
		esp_info("Drop delivery of stale staged DISCONNECT (seq=%u conn=%u staged_assoc=%u)\n",
			 disc_seq, priv->conn_generation, priv->staged_assoc_seq);
		return;
	}

	priv->local_disconnect_req = false;
	priv->disconnect_awaiting_mlme = false;
	priv->disconnect_cmd_seq = 0;
	priv->staged_assoc_seq = 0;
	priv->assoc_awaiting_mlme = false;
	priv->assoc_cmd_seq = 0;
	priv->conn_generation = 0;
	memset(priv->disconnect_bssid, 0, MAC_ADDR_LEN);
	staged_assoc = priv->staged_assoc_skb;
	priv->staged_assoc_skb = NULL;
	spin_unlock_bh(&priv->bss_lock);

	kfree_skb(staged_assoc);

	memcpy(ssid, event->ssid, MAX_SSID_LEN);
	ssid[MAX_SSID_LEN] = '\0';

	esp_info("Disconnect event for ssid %s [reason:%d]\n",
			ssid, event->reason);

	esp_wifi_put_bss(priv);
	esp_wifi_clear_assoc_pending(priv);
	esp_wifi_clear_auth_pending(priv);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0))
	if (priv->adapter)
		cfg80211_bss_flush(priv->adapter->wiphy);
#endif
	esp_port_close(priv);
	priv->stop_data = 1;

	if (local)
		CFG80211_DISCONNECTED(priv->ndev, event->reason, NULL, 0, true, GFP_KERNEL);
	else
		process_deauth_event(priv, event);
}

static bool esp_mlme_busy(struct esp_wifi_device *priv)
{
	return priv->mlme_wdev_held || priv->auth_cmd_pending ||
	       priv->assoc_cmd_pending || priv->disconnect_cmd_pending;
}

/* AUTH_RX frames received while cfg80211 still owns the AUTH command must not
 * be published until the command outcome is known. */
static void esp_auth_splice_guard(struct sk_buff_head *list,
				  struct sk_buff_head *head,
				  struct esp_wifi_device *priv)
{
	bool cleanup = false;

	if (priv && priv->adapter)
		cleanup = test_bit(ESP_CLEANUP_IN_PROGRESS,
				   &priv->adapter->state_flags) ||
			  test_bit(ESP_DRIVER_UNLOADING,
				   &priv->adapter->state_flags);

	if (priv && list == &priv->staged_auth_q) {
		if (priv->auth_cmd_pending && !cleanup)
			return;
		if (!priv->auth_awaiting_mlme) {
			skb_queue_purge(list);
			return;
		}
	}

	skb_queue_splice_tail_init(list, head);
}

static void esp_mlme_flush_staged(struct esp_wifi_device *priv)
{
	struct sk_buff *auth, *assoc, *disc;
	struct sk_buff_head auth_q;

	skb_queue_head_init(&auth_q);

	spin_lock_bh(&priv->bss_lock);
	esp_auth_splice_guard(&priv->staged_auth_q, &auth_q, priv);
	assoc = priv->staged_assoc_skb;
	priv->staged_assoc_skb = NULL;
	disc = priv->staged_disconnect_skb;
	priv->staged_disconnect_skb = NULL;
	spin_unlock_bh(&priv->bss_lock);

	while ((auth = skb_dequeue(&auth_q))) {
		esp_deliver_auth_from_skb(priv, auth);
		kfree_skb(auth);
	}
	if (assoc) {
		esp_deliver_assoc_from_skb(priv, assoc);
		kfree_skb(assoc);
	}
	if (disc) {
		esp_deliver_disconnect_from_skb(priv, disc);
		kfree_skb(disc);
	}

	spin_lock_bh(&priv->bss_lock);
	if (!priv->assoc_awaiting_mlme && !priv->auth_awaiting_mlme && !priv->disconnect_awaiting_mlme)
		cancel_delayed_work(&priv->mlme_timeout_work);
	spin_unlock_bh(&priv->bss_lock);
}

static void esp_mlme_work(struct work_struct *work)
{
	struct esp_wifi_device *priv = container_of(work, struct esp_wifi_device,
						    mlme_work);

	if (!priv || !priv->ndev)
		return;

	spin_lock_bh(&priv->bss_lock);
	if (priv->mlme_wdev_held) {
		spin_unlock_bh(&priv->bss_lock);
		return;
	}
	spin_unlock_bh(&priv->bss_lock);

	esp_wdev_lock(&priv->wdev);
	esp_mlme_flush_staged(priv);
	esp_wdev_unlock(&priv->wdev);
}

static void esp_mlme_timeout_work(struct work_struct *work)
{
	struct esp_wifi_device *priv = container_of(work, struct esp_wifi_device,
						    mlme_timeout_work.work);
	bool assoc_timed_out = false;
	bool disc_timed_out = false;
	bool auth_timed_out = false;

	if (!priv || !priv->adapter)
		return;

	spin_lock_bh(&priv->bss_lock);
	if (priv->assoc_awaiting_mlme) {
		priv->assoc_awaiting_mlme = false;
		priv->assoc_cmd_pending = false;
		priv->staged_assoc_seq = 0;
		priv->assoc_cmd_seq = 0;
		assoc_timed_out = true;
	}
	if (priv->disconnect_awaiting_mlme) {
		priv->disconnect_awaiting_mlme = false;
		priv->disconnect_cmd_pending = false;
		priv->disconnect_cmd_seq = 0;
		disc_timed_out = true;
	}
	if (priv->auth_awaiting_mlme) {
		priv->auth_awaiting_mlme = false;
		priv->auth_cmd_pending = false;
		priv->auth_cmd_seq = 0;
		auth_timed_out = true;
	}
	spin_unlock_bh(&priv->bss_lock);

	if (assoc_timed_out) {
		esp_err("MLME ASSOC timeout awaiting terminal event from firmware\n");
		esp_wifi_clear_assoc_pending(priv);
		if (priv->adapter) {
			esp_schedule_fw_reset_recovery(priv->adapter);
			esp_request_firmware_restart(priv->adapter);
		}
	}
	if (disc_timed_out) {
		esp_err("MLME DISCONNECT timeout awaiting terminal event from firmware\n");
		esp_wifi_put_bss(priv);
		esp_wifi_clear_assoc_pending(priv);
		esp_wifi_clear_auth_pending(priv);
		esp_port_close(priv);
		if (priv->ndev)
			CFG80211_DISCONNECTED(priv->ndev, WLAN_REASON_UNSPECIFIED, NULL, 0, true, GFP_KERNEL);
		if (priv->adapter) {
			esp_schedule_fw_reset_recovery(priv->adapter);
			esp_request_firmware_restart(priv->adapter);
		}
	}
	if (auth_timed_out) {
		esp_err("MLME AUTH timeout awaiting terminal event from firmware\n");
		esp_wifi_clear_auth_pending(priv);
		if (priv->adapter) {
			esp_schedule_fw_reset_recovery(priv->adapter);
			esp_request_firmware_restart(priv->adapter);
		}
	}
}

static void esp_scan_timeout_work(struct work_struct *work)
{
	struct esp_wifi_device *priv = container_of(work, struct esp_wifi_device,
						    scan_timeout_work.work);
	struct cfg80211_scan_request *req;

	if (!priv)
		return;

	if (priv->scan_in_progress || priv->request) {
		req = esp_mark_scan_done(priv, true);
		if (priv->waiting_for_scan_done) {
			priv->waiting_for_scan_done = false;
			wake_up_interruptible(&priv->wait_for_scan_completion);
		}
		if (req && priv->adapter) {
			esp_warn("Scan timeout awaiting scan done from firmware, aborting scan\n");
			esp_schedule_fw_reset_recovery(priv->adapter);
			esp_request_firmware_restart(priv->adapter);
		}
	}
}

void esp_mlme_init(struct esp_wifi_device *priv)
{
	if (!priv)
		return;
	skb_queue_head_init(&priv->staged_auth_q);
	INIT_WORK(&priv->mlme_work, esp_mlme_work);
	INIT_DELAYED_WORK(&priv->mlme_timeout_work, esp_mlme_timeout_work);
	INIT_DELAYED_WORK(&priv->scan_timeout_work, esp_scan_timeout_work);
}

static bool esp_mlme_has_staged(struct esp_wifi_device *priv)
{
	return !skb_queue_empty(&priv->staged_auth_q) ||
	       priv->staged_assoc_skb || priv->staged_disconnect_skb;
}

static void esp_mlme_begin(struct esp_wifi_device *priv, enum esp_mlme_op op)
{
	if (!priv)
		return;

	spin_lock_bh(&priv->bss_lock);
	priv->mlme_wdev_held = true;
	switch (op) {
	case ESP_MLME_AUTH:
		priv->auth_cmd_pending = true;
		priv->auth_awaiting_mlme = false;
		break;
	case ESP_MLME_ASSOC:
		priv->assoc_mlme_notified = false;
		break;
	case ESP_MLME_DISCONNECT:
		priv->disconnect_cmd_pending = true;
		priv->disconnect_awaiting_mlme = false;
		break;
	}
	spin_unlock_bh(&priv->bss_lock);
}

static void esp_mlme_end(struct esp_wifi_device *priv, enum esp_mlme_op op,
			 bool success)
{
	bool need_work = false;

	if (!priv)
		return;

	if (!success && op == ESP_MLME_AUTH) {
		spin_lock_bh(&priv->bss_lock);
		skb_queue_purge(&priv->staged_auth_q);
		priv->auth_cmd_pending = false;
		priv->auth_awaiting_mlme = false;
		priv->auth_cmd_seq = 0;
		memset(priv->auth_bssid, 0, MAC_ADDR_LEN);
		spin_unlock_bh(&priv->bss_lock);
	}

	if (!success && op == ESP_MLME_ASSOC) {
		struct sk_buff *drop;

		spin_lock_bh(&priv->bss_lock);
		drop = priv->staged_assoc_skb;
		priv->staged_assoc_skb = NULL;
		priv->staged_assoc_seq = 0;
		priv->assoc_awaiting_mlme = false;
		priv->assoc_mlme_notified = false;
		spin_unlock_bh(&priv->bss_lock);
		kfree_skb(drop);
	}

	esp_mlme_flush_staged(priv);

	spin_lock_bh(&priv->bss_lock);
	switch (op) {
	case ESP_MLME_AUTH:
		priv->auth_cmd_pending = false;
		priv->auth_awaiting_mlme = success;
		break;
	case ESP_MLME_ASSOC:
		priv->assoc_cmd_pending = false;
		if (success && !priv->assoc_mlme_notified && priv->assoc_cmd_seq)
			priv->assoc_awaiting_mlme = true;
		else
			priv->assoc_awaiting_mlme = false;
		break;
	case ESP_MLME_DISCONNECT:
		priv->disconnect_cmd_pending = false;
		priv->disconnect_awaiting_mlme = success;
		break;
	}
	priv->mlme_wdev_held = false;
	need_work = esp_mlme_has_staged(priv);
	if (priv->assoc_awaiting_mlme || priv->auth_awaiting_mlme || priv->disconnect_awaiting_mlme)
		schedule_delayed_work(&priv->mlme_timeout_work, msecs_to_jiffies(5000));
	else
		cancel_delayed_work(&priv->mlme_timeout_work);
	spin_unlock_bh(&priv->bss_lock);

	if (need_work && priv->adapter && priv->adapter->events_wq)
		queue_work(priv->adapter->events_wq, &priv->mlme_work);
}

static void esp_mlme_stage_event(struct esp_wifi_device *priv,
				 struct sk_buff *skb, u8 event_code)
{
	struct sk_buff *copy;

	copy = skb_copy(skb, GFP_ATOMIC);
	if (!copy) {
		esp_err("Failed to stage MLME event %u\n", event_code);
		spin_lock_bh(&priv->bss_lock);
		if (event_code == EVENT_ASSOC_RX) {
			priv->assoc_awaiting_mlme = false;
			priv->assoc_cmd_pending = false;
			priv->staged_assoc_seq = 0;
			priv->assoc_cmd_seq = 0;
		} else if (event_code == EVENT_STA_DISCONNECT) {
			priv->disconnect_awaiting_mlme = false;
			priv->disconnect_cmd_pending = false;
			priv->disconnect_cmd_seq = 0;
		} else if (event_code == EVENT_AUTH_RX) {
			priv->auth_awaiting_mlme = false;
			priv->auth_cmd_pending = false;
			priv->auth_cmd_seq = 0;
		}
		spin_unlock_bh(&priv->bss_lock);
		if (priv->adapter) {
			esp_schedule_fw_reset_recovery(priv->adapter);
			esp_request_firmware_restart(priv->adapter);
		}
		return;
	}

	spin_lock_bh(&priv->bss_lock);
	switch (event_code) {
	case EVENT_AUTH_RX:
		if (skb_queue_len(&priv->staged_auth_q) < 8)
			skb_queue_tail(&priv->staged_auth_q, copy);
		else
			kfree_skb(copy);
		copy = NULL;
		break;
	case EVENT_ASSOC_RX:
		if (!priv->staged_assoc_skb) {
			priv->staged_assoc_skb = copy;
			priv->staged_assoc_seq = priv->assoc_cmd_seq;
			copy = NULL;
		}
		break;
	case EVENT_STA_DISCONNECT:
		if (!priv->staged_disconnect_skb) {
			priv->staged_disconnect_skb = copy;
			copy = NULL;
		}
		break;
	default:
		break;
	}
	spin_unlock_bh(&priv->bss_lock);
	kfree_skb(copy);
}

void esp_mlme_cancel(struct esp_wifi_device *priv)
{
	struct sk_buff *assoc_skb;
	struct sk_buff *disc_skb;
	struct sk_buff *skb;
	struct sk_buff_head auth_q;

	if (!priv)
		return;

	cancel_delayed_work_sync(&priv->mlme_timeout_work);
	cancel_delayed_work_sync(&priv->scan_timeout_work);
	cancel_work_sync(&priv->mlme_work);

	skb_queue_head_init(&auth_q);
	spin_lock_bh(&priv->bss_lock);
	esp_auth_splice_guard(&priv->staged_auth_q, &auth_q, priv);
	assoc_skb = priv->staged_assoc_skb;
	priv->staged_assoc_skb = NULL;
	priv->staged_assoc_seq = 0;
	disc_skb = priv->staged_disconnect_skb;
	priv->staged_disconnect_skb = NULL;
	priv->assoc_awaiting_mlme = false;
	priv->auth_awaiting_mlme = false;
	priv->disconnect_awaiting_mlme = false;
	priv->mlme_wdev_held = false;
	priv->auth_cmd_pending = false;
	priv->disconnect_cmd_pending = false;
	priv->conn_generation = 0;
	priv->local_disconnect_req = false;
	priv->disconnect_cmd_seq = 0;
	memset(priv->disconnect_bssid, 0, MAC_ADDR_LEN);
	spin_unlock_bh(&priv->bss_lock);

	while ((skb = skb_dequeue(&auth_q)))
		kfree_skb(skb);
	kfree_skb(assoc_skb);
	kfree_skb(disc_skb);

	esp_wifi_clear_assoc_pending(priv);
	esp_wifi_clear_auth_pending(priv);
	if (priv->scan_in_progress || priv->request)
		ESP_MARK_SCAN_DONE(priv, true);
}

static int handle_mgmt_tx_done(struct esp_wifi_device *priv,
				struct command_node *cmd_node);
static void recycle_cmd_node(struct esp_adapter *adapter,
		struct command_node *cmd_node);

int internal_scan_request(struct esp_wifi_device *priv, char *ssid,
		uint8_t channel, uint8_t is_blocking);

struct beacon_probe_fixed_params {
	__le64 timestamp;
	__le16 beacon_interval;
	__le16 cap_info;
} __packed;

static bool esp_cmd_is_admitted(struct esp_adapter *adapter, u8 cmd_code)
{
	if (!adapter)
		return false;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags))
		return cmd_code == CMD_DEINIT_INTERFACE &&
		       test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags);
	if (test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags))
		return cmd_code == CMD_INIT_INTERFACE ||
		       cmd_code == CMD_GET_MAC ||
		       cmd_code == CMD_DEINIT_INTERFACE ||
		       cmd_code == CMD_START_OTA_UPDATE ||
		       cmd_code == CMD_START_OTA_WRITE ||
		       cmd_code == CMD_START_OTA_END ||
		       cmd_code == CMD_RAW_TP_ESP_TO_HOST ||
		       cmd_code == CMD_RAW_TP_HOST_TO_ESP;
	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags))
		return cmd_code == CMD_DEINIT_INTERFACE &&
		       test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags);
	return true;
}

static struct command_node *get_free_cmd_node(struct esp_adapter *adapter, u8 cmd_code)
{
	struct command_node *cmd_node;

	spin_lock_bh(&adapter->cmd_lock);
	if (!test_bit(ESP_CMD_INIT_DONE, &adapter->state_flags)) {
		spin_unlock_bh(&adapter->cmd_lock);
		return ERR_PTR(-EBUSY);
	}

	if (!esp_cmd_is_admitted(adapter, cmd_code)) {
		spin_unlock_bh(&adapter->cmd_lock);
		return ERR_PTR(-EBUSY);
	}

	spin_lock_bh(&adapter->cmd_free_queue_lock);

	if (list_empty(&adapter->cmd_free_queue)) {
		spin_unlock_bh(&adapter->cmd_free_queue_lock);
		spin_unlock_bh(&adapter->cmd_lock);
		esp_err("No free cmd node found\n");
		return ERR_PTR(-ENOMEM);
	}
	cmd_node = list_first_entry(&adapter->cmd_free_queue,
				    struct command_node, list);
	list_del_init(&cmd_node->list);
	cmd_node->in_pending_queue = false;
	cmd_node->in_use = true;
	atomic_inc(&adapter->cmd_nodes_in_use);
	spin_unlock_bh(&adapter->cmd_free_queue_lock);
	spin_unlock_bh(&adapter->cmd_lock);

	cmd_node->cmd_skb = esp_if_alloc_skb(adapter, ESP_SIZE_OF_CMD_NODE);
	if (!cmd_node->cmd_skb) {
		esp_err("No free cmd node skb found\n");
		recycle_cmd_node(adapter, cmd_node);
		return NULL;
	}

	return cmd_node;
}

static inline void reset_cmd_node(struct esp_adapter *adapter, struct command_node *cmd_node)
{
	spin_lock_bh(&adapter->cmd_lock);

	spin_lock_bh(&adapter->cmd_pending_queue_lock);
	if (cmd_node->in_pending_queue) {
		esp_verbose("recycling command still in cmd queue\n");
		list_del_init(&cmd_node->list);
		cmd_node->in_pending_queue = false;
	}
	spin_unlock_bh(&adapter->cmd_pending_queue_lock);
	cmd_node->cmd_code = 0;
	cmd_node->cmd_seq = 0;
	cmd_node->completed = false;
	cmd_node->result = 0;
	cmd_node->queued_at = 0;
	cmd_node->sent_at = 0;
	cmd_node->cookie = 0;
	cmd_node->mgmt_dont_wait_for_ack = false;
	cmd_node->mgmt_frame_len = 0;
	if (cmd_node->mgmt_frame) {
		kfree(cmd_node->mgmt_frame);
		cmd_node->mgmt_frame = NULL;
	}
	if (cmd_node->cmd_skb) {
		dev_kfree_skb_any(cmd_node->cmd_skb);
		cmd_node->cmd_skb = NULL;
	}
	if (cmd_node->resp_skb) {
		dev_kfree_skb_any(cmd_node->resp_skb);
		cmd_node->resp_skb = NULL;
	}
	spin_unlock_bh(&adapter->cmd_lock);
}

static int queue_cmd_node(struct esp_adapter *adapter,
			 struct command_node *cmd_node, u8 flag_high_prio)
{
	if (!adapter || !cmd_node)
		return -EINVAL;

	spin_lock_bh(&adapter->cmd_lock);
	spin_lock_bh(&adapter->cmd_pending_queue_lock);

	if (!esp_cmd_is_admitted(adapter, cmd_node->cmd_code)) {
		cmd_node->result = -ESHUTDOWN;
		cmd_node->completed = true;
		spin_unlock_bh(&adapter->cmd_pending_queue_lock);
		spin_unlock_bh(&adapter->cmd_lock);

		esp_dbg("CMD_REJECT_SHUTDOWN code=%u seq=%u\n",
			cmd_node->cmd_code, cmd_node->cmd_seq);
		wake_up_all(&adapter->wait_for_cmd_resp);
		return -ESHUTDOWN;
	}

	cmd_node->queued_at = jiffies;
	if (flag_high_prio)
		list_add_rcu(&cmd_node->list, &adapter->cmd_pending_queue);
	else
		list_add_tail_rcu(&cmd_node->list, &adapter->cmd_pending_queue);
	cmd_node->in_pending_queue = true;

	if (adapter->cmd_wq)
		queue_work(adapter->cmd_wq, &adapter->cmd_work);

	spin_unlock_bh(&adapter->cmd_pending_queue_lock);
	spin_unlock_bh(&adapter->cmd_lock);

	esp_dbg("CMD_QUEUE code=%u seq=%u high=%u\n",
			cmd_node->cmd_code, cmd_node->cmd_seq, flag_high_prio);
	return 0;
}

static int decode_mac_addr(struct esp_wifi_device *priv,
		struct command_node *cmd_node)
{
	int ret = 0;
	struct cmd_config_mac_address *header;

	if (!priv || !cmd_node ||
	    !cmd_node->resp_skb ||
	    !cmd_node->resp_skb->data ||
	    cmd_node->resp_skb->len < (sizeof(struct command_header) + MAC_ADDR_LEN) ||
	    esp_wire_le16_to_cpu(((struct command_header *)cmd_node->resp_skb->data)->len) < MAC_ADDR_LEN) {
		esp_err("Invalid mac addr response\n");
		return -EINVAL;
	}

	header = (struct cmd_config_mac_address *) (cmd_node->resp_skb->data);

	if (header->header.cmd_status != CMD_RESPONSE_SUCCESS) {
		esp_info("Command failed\n");
		ret = -1;
	} else if (priv) {
		memcpy(priv->mac_address, header->mac_addr, MAC_ADDR_LEN);
	} else {
		esp_err("priv not updated\n");
	}

	return ret;
}

static int decode_rssi(struct esp_wifi_device *priv,
		struct command_node *cmd_node)
{
	int ret = 0;
	struct command_header *header;
	int8_t *rssi;

	if (!priv || !cmd_node ||
	    !cmd_node->resp_skb ||
	    !cmd_node->resp_skb->data ||
	    cmd_node->resp_skb->len < (sizeof(struct command_header) + sizeof(int8_t)) ||
	    esp_wire_le16_to_cpu(((struct command_header *)cmd_node->resp_skb->data)->len) < sizeof(int8_t)) {
		esp_err("Invalid rssi response\n");
		return -EINVAL;
	}

	header = (struct command_header *) (cmd_node->resp_skb->data);

	if (header->cmd_status != CMD_RESPONSE_SUCCESS) {
		esp_info("Command failed\n");
		ret = -1;
	}

	rssi = (int8_t *)(cmd_node->resp_skb->data + sizeof(struct command_header));

	if (priv)
		priv->rssi = *rssi;
	else
		esp_err("priv not updated\n");

	return ret;
}

static int decode_tx_power(struct esp_wifi_device *priv,
		struct command_node *cmd_node)
{
	int ret = 0;
	struct cmd_set_get_val *header;

	if (!priv || !cmd_node ||
	    !cmd_node->resp_skb ||
	    !cmd_node->resp_skb->data ||
	    cmd_node->resp_skb->len < sizeof(struct cmd_set_get_val) ||
	    esp_wire_le16_to_cpu(((struct command_header *)cmd_node->resp_skb->data)->len) < sizeof(uint32_t)) {
		esp_err("Invalid tx power response\n");
		return -EINVAL;
	}

	header = (struct cmd_set_get_val *) (cmd_node->resp_skb->data);

	if (header->header.cmd_status != CMD_RESPONSE_SUCCESS) {
		esp_info("Command failed\n");
		ret = -1;
	}

	if (priv)
		priv->tx_pwr = header->value;
	else
		esp_err("priv not updated\n");

	return ret;
}

static int decode_disconnect_resp(struct esp_wifi_device *priv, struct command_node *cmd_node)
{
	int ret = 0;
	struct command_header *cmd;

	if (!cmd_node || !cmd_node->resp_skb || !cmd_node->resp_skb->data ||
	    cmd_node->resp_skb->len < sizeof(struct command_header)) {
		esp_info("Failed. cmd_node:%p\n", cmd_node);
		if (cmd_node)
			esp_info("code: %u resp_skb:%p\n",
					cmd_node->cmd_code, cmd_node->resp_skb);
		return -1;
	}

	cmd = (struct command_header *) (cmd_node->resp_skb->data);

	if (cmd->cmd_status == CMD_RESPONSE_UNSUPPORTED) {
		ret = -EOPNOTSUPP;
	} else if (cmd->cmd_status != CMD_RESPONSE_SUCCESS) {
		esp_info("[0x%x] Command failed\n", cmd_node->cmd_code);
		ret = -1;
	}

	if (priv) {
		if (ret)
			priv->local_disconnect_req = false;
		else
			priv->local_disconnect_req = true;
	} else {
		esp_err("priv not updated\n");
	}

	return ret;
}


static int decode_common_resp(struct command_node *cmd_node)
{
	int ret = 0;
	struct command_header *cmd;


	if (!cmd_node || !cmd_node->resp_skb || !cmd_node->resp_skb->data ||
	    cmd_node->resp_skb->len < sizeof(struct command_header)) {

		esp_info("Failed. cmd_node:%p\n", cmd_node);

		if (cmd_node)
			esp_info("code: %u resp_skb:%p\n",
				 cmd_node->cmd_code, cmd_node->resp_skb);

		return -1;
	}

	cmd = (struct command_header *) (cmd_node->resp_skb->data);

	if (cmd->cmd_status != CMD_RESPONSE_SUCCESS) {
		esp_info("[0x%x] Command failed\n", cmd_node->cmd_code);
		ret = -1;
	}

	return ret;
}

static void recycle_cmd_node(struct esp_adapter *adapter,
		struct command_node *cmd_node)
{
	bool was_in_use;

	if (!adapter || !cmd_node)
		return;

	was_in_use = cmd_node->in_use;
	reset_cmd_node(adapter, cmd_node);
	cmd_node->in_use = false;

	spin_lock_bh(&adapter->cmd_free_queue_lock);
	list_add_tail(&cmd_node->list, &adapter->cmd_free_queue);
	spin_unlock_bh(&adapter->cmd_free_queue_lock);

	if (was_in_use) {
		atomic_dec(&adapter->cmd_nodes_in_use);
		wake_up_all(&adapter->wait_for_cmd_resp);
	}
}


static bool esp_cmd_timeout_is_commit_ambiguous(u8 cmd_code)
{
	switch (cmd_code) {
	case CMD_INIT_INTERFACE:
	case CMD_DEINIT_INTERFACE:
	case CMD_SET_MAC:
	case CMD_SCAN_REQUEST:
	case CMD_STA_CONNECT:
	case CMD_DISCONNECT:
	case CMD_ADD_KEY:
	case CMD_DEL_KEY:
	case CMD_SET_DEFAULT_KEY:
	case CMD_STA_AUTH:
	case CMD_STA_ASSOC:
	case CMD_STA_SET_AUTHORIZED:
	case CMD_SET_IP_ADDR:
	case CMD_SET_MCAST_MAC_ADDR:
	case CMD_SET_TXPOWER:
	case CMD_SET_REG_DOMAIN:
	case CMD_RAW_TP_ESP_TO_HOST:
	case CMD_RAW_TP_HOST_TO_ESP:
	case CMD_SET_WOW_CONFIG:
	case CMD_SET_MODE:
	case CMD_SET_IE:
	case CMD_AP_CONFIG:
	case CMD_MGMT_TX:
	case CMD_AP_STATION:
	case CMD_SET_TIME:
	case CMD_START_OTA_UPDATE:
	case CMD_START_OTA_WRITE:
	case CMD_START_OTA_END:
		return true;
	default:
		return false;
	}
}

static bool esp_cmd_timeout_needs_restart(u8 cmd_code)
{
	return esp_cmd_timeout_is_commit_ambiguous(cmd_code);
}

static long esp_cmd_wait_event_guard(struct esp_adapter *adapter,
				     struct command_node *cmd_node,
				     unsigned long timeout)
{
	long ret;
	bool restart = false;

	ret = wait_event_timeout(adapter->wait_for_cmd_resp,
				 READ_ONCE(cmd_node->completed), timeout);
	if (ret > 0)
		return ret;

	/* On timeout, synchronize with cmd_work and pending queue under lock */
	spin_lock_bh(&adapter->cmd_lock);
	if (cmd_node->completed) {
		ret = 1;
	} else {
		/* Safely retire command if still queued in pending queue before cmd_work can submit it */
		spin_lock_bh(&adapter->cmd_pending_queue_lock);
		if (cmd_node->in_pending_queue) {
			list_del_init(&cmd_node->list);
			cmd_node->in_pending_queue = false;
			cmd_node->result = -ETIMEDOUT;
			cmd_node->completed = true;
			ret = 0;
		}
		spin_unlock_bh(&adapter->cmd_pending_queue_lock);

		/* If the command was submitted or is active, and is commit-ambiguous,
		 * quarantine the incarnation immediately before cur_cmd can be cleared. */
		if (cmd_node->sent_at &&
		    esp_cmd_timeout_is_commit_ambiguous(cmd_node->cmd_code) &&
		    !test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) &&
		    !test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) &&
		    !test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags) &&
		    !test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags)) {
			restart = true;
		}
	}
	spin_unlock_bh(&adapter->cmd_lock);

	if (restart) {
		esp_err("CMD_TIMEOUT_QUARANTINE code=%u seq=%u\n",
			cmd_node->cmd_code, cmd_node->cmd_seq);
		esp_schedule_fw_reset_recovery(adapter);
		esp_request_firmware_restart(adapter);
	}

	return ret;
}

static int wait_and_decode_cmd_resp(struct esp_wifi_device *priv,
			struct command_node *cmd_node)
{
	struct esp_adapter *adapter = NULL;
	unsigned long elapsed_ms = 0;
	unsigned long sent_ms = 0;
	bool was_current = false;
	int ret = 0;

	if (!priv || !priv->adapter || !cmd_node) {
		esp_info("Invalid params\n");
		if (priv && priv->adapter) {
			adapter = priv->adapter;
			if (cmd_node)
				recycle_cmd_node(adapter, cmd_node);
		}
		return -EINVAL;
	}

	adapter = priv->adapter;

	/* Once submitted, command ownership is protocol state. Do not let a
	 * userspace signal abandon a command that firmware may still execute. */
	ret = esp_cmd_wait_event_guard(adapter, cmd_node, COMMAND_RESPONSE_TIMEOUT);
	if (cmd_node->queued_at)
		elapsed_ms = jiffies_to_msecs(jiffies - cmd_node->queued_at);
	if (cmd_node->sent_at)
		sent_ms = jiffies_to_msecs(jiffies - cmd_node->sent_at);

	spin_lock_bh(&adapter->cmd_lock);
	/* A response can win the race at the timeout boundary after the wait
	 * returned zero. Recheck completion while holding the same lock used by
	 * process_cmd_resp() before declaring the command lost. */
	if (ret <= 0 && cmd_node->completed)
		ret = 1;

	was_current = adapter->cur_cmd == cmd_node;
	if (was_current) {
		adapter->cur_cmd = NULL;
		adapter->cmd_resp = 0;
		adapter->cmd_resp_seq = 0;
	} else if (cmd_node->sent_at && adapter->cur_cmd) {
		esp_err("CMD_STATE_MISMATCH done=%u/%u current=%u/%u\n",
				cmd_node->cmd_code, cmd_node->cmd_seq,
				adapter->cur_cmd->cmd_code, adapter->cur_cmd->cmd_seq);
	}

	/* Remove a command that expired while waiting behind another command.
	 * This closes the gap in which cmd_work could otherwise submit it after
	 * its caller had already returned and recycled the node. */
	if (ret <= 0 && cmd_node->in_pending_queue) {
		spin_lock_bh(&adapter->cmd_pending_queue_lock);
		if (cmd_node->in_pending_queue) {
			list_del_init(&cmd_node->list);
			cmd_node->in_pending_queue = false;
		}
		spin_unlock_bh(&adapter->cmd_pending_queue_lock);
	}

	if (ret == 0) {
		cmd_node->result = -ETIMEDOUT;
		cmd_node->completed = true;
		ret = -ETIMEDOUT;
	} else if (ret > 0) {
		ret = cmd_node->result;
	}
	spin_unlock_bh(&adapter->cmd_lock);

	if (ret == -ETIMEDOUT)
		esp_err("CMD_TIMEOUT code=%u seq=%u stage=%s elapsed_ms=%lu sent_ms=%lu\n",
				cmd_node->cmd_code, cmd_node->cmd_seq,
				cmd_node->sent_at ? "transport" : "pending-queue",
				elapsed_ms, sent_ms);
	else if (ret < 0)
		esp_err("CMD_WAIT_FAILED code=%u seq=%u ret=%d elapsed_ms=%lu sent_ms=%lu\n",
				cmd_node->cmd_code, cmd_node->cmd_seq, ret,
				elapsed_ms, sent_ms);
	else {
		esp_dbg("CMD_COMPLETE code=%u seq=%u result=%d elapsed_ms=%lu sent_ms=%lu\n",
				cmd_node->cmd_code, cmd_node->cmd_seq, ret,
				elapsed_ms, sent_ms);
	}

	if (ret == -ETIMEDOUT && cmd_node->sent_at &&
	    esp_cmd_timeout_needs_restart(cmd_node->cmd_code) &&
	    !test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) &&
	    !test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) &&
	    !test_bit(ESP_OTA_IN_PROGRESS, &adapter->state_flags) &&
	    !test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags) &&
	    !test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags)) {
		esp_err("CMD_TIMEOUT_RESTART code=%u seq=%u: submitted command state unknown\n",
			cmd_node->cmd_code, cmd_node->cmd_seq);
		esp_schedule_fw_reset_recovery(adapter);
		esp_request_firmware_restart(adapter);
	}

	if (!test_bit(ESP_DRIVER_ACTIVE, &adapter->state_flags) && !ret)
		ret = -ESHUTDOWN;

	switch (cmd_node->cmd_code) {

	case CMD_SCAN_REQUEST:
		if (ret == 0)
			ret = decode_common_resp(cmd_node);

		if (ret) {
			priv->waiting_for_scan_done = false;
			priv->scan_in_progress = false;
			priv->request = NULL;
			/* Non-zero from .scan means cfg80211 still owns the
			 * request and will free it. Do not cfg80211_scan_done(). */
			wake_up_interruptible(&priv->wait_for_scan_completion);
		}
		break;

	case CMD_INIT_INTERFACE:
	case CMD_DEINIT_INTERFACE:
	case CMD_STA_AUTH:
	case CMD_STA_ASSOC:
	case CMD_STA_SET_AUTHORIZED:
	case CMD_STA_CONNECT:
	case CMD_ADD_KEY:
	case CMD_DEL_KEY:
	case CMD_SET_MODE:
	case CMD_SET_IE:
	case CMD_AP_CONFIG:
	case CMD_AP_STATION:
	case CMD_SET_DEFAULT_KEY:
	case CMD_SET_IP_ADDR:
	case CMD_SET_MCAST_MAC_ADDR:
	case CMD_GET_REG_DOMAIN:
	case CMD_SET_REG_DOMAIN:
	case CMD_RAW_TP_ESP_TO_HOST:
	case CMD_RAW_TP_HOST_TO_ESP:
	case CMD_SET_WOW_CONFIG:
	case CMD_SET_TIME:
	case CMD_START_OTA_UPDATE:
	case CMD_START_OTA_WRITE:
	case CMD_START_OTA_END:
		/* intentional fallthrough */
		if (ret == 0)
			ret = decode_common_resp(cmd_node);
		break;

	case CMD_GET_MAC:
	case CMD_SET_MAC:
		if (ret == 0)
			ret = decode_mac_addr(priv, cmd_node);
		break;
	case CMD_DISCONNECT:
		if (ret == 0)
			ret = decode_disconnect_resp(priv, cmd_node);
		break;
	case CMD_GET_TXPOWER:
	case CMD_SET_TXPOWER:
		if (ret == 0)
			ret = decode_tx_power(priv, cmd_node);
		break;
        case CMD_STA_RSSI:
		if (ret == 0)
			ret = decode_rssi(priv, cmd_node);
		break;
	case CMD_MGMT_TX:
		if (ret == 0)
			ret = handle_mgmt_tx_done(priv, cmd_node);
		break;
	default:
		esp_info("Resp for [0x%x] ignored\n", cmd_node->cmd_code);
		ret = -EINVAL;
		break;
	}

	recycle_cmd_node(adapter, cmd_node);

	/* The completed/timed-out node is no longer current or pending. This is
	 * the only safe point to start the next command. */
	if (adapter->cmd_wq && test_bit(ESP_CMD_INIT_DONE, &adapter->state_flags))
		queue_work(adapter->cmd_wq, &adapter->cmd_work);
	return ret;
}

static void free_esp_cmd_pool(struct esp_adapter *adapter)
{
	int i;
	struct command_node *cmd_pool = NULL;

	if (!adapter || !adapter->cmd_pool)
		return;

	cmd_pool = adapter->cmd_pool;

	for (i = 0; i < ESP_NUM_OF_CMD_NODES; i++) {

		spin_lock_bh(&adapter->cmd_lock);
		if (cmd_pool[i].resp_skb) {
			dev_kfree_skb_any(cmd_pool[i].resp_skb);
			cmd_pool[i].resp_skb = NULL;
		}
		if (cmd_pool[i].cmd_skb) {
			dev_kfree_skb_any(cmd_pool[i].cmd_skb);
			cmd_pool[i].cmd_skb = NULL;
		}
		spin_unlock_bh(&adapter->cmd_lock);
	}

	kfree(adapter->cmd_pool);
	adapter->cmd_pool = NULL;
}

static int alloc_esp_cmd_pool(struct esp_adapter *adapter)
{
	u16 i;

	struct command_node *cmd_pool = kcalloc(ESP_NUM_OF_CMD_NODES,
		sizeof(struct command_node), GFP_KERNEL);

	if (!cmd_pool)
		return -ENOMEM;

	adapter->cmd_pool = cmd_pool;

	for (i = 0; i < ESP_NUM_OF_CMD_NODES; i++) {

		cmd_pool[i].cmd_skb = NULL;
		cmd_pool[i].resp_skb = NULL;
		recycle_cmd_node(adapter, &cmd_pool[i]);
	}

	return 0;
}

static void esp_cmd_work(struct work_struct *work)
{
	int ret;
	struct command_node *cmd_node = NULL;
	struct esp_adapter *adapter = NULL;
	struct esp_payload_header *payload_header = NULL;

	adapter = esp_get_adapter();

	if (!adapter)
		return;

	if (!test_bit(ESP_DRIVER_ACTIVE, &adapter->state_flags))
		return;

	spin_lock_bh(&adapter->cmd_lock);
	/* This is the authoritative submit gate. A worker queued before cleanup
	 * must not dequeue or submit after teardown published its state. */
	if (!test_bit(ESP_CMD_INIT_DONE, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
	    (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags)) ||
	    (test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags))) {
		spin_unlock_bh(&adapter->cmd_lock);
		return;
	}
	if (adapter->cur_cmd) {
		/* The current command's waiter starts us again after clearing cur_cmd. */
		esp_verbose("CMD_WORK_BUSY current=%u/%u\n",
				adapter->cur_cmd->cmd_code, adapter->cur_cmd->cmd_seq);
		spin_unlock_bh(&adapter->cmd_lock);
		return;
	}

	spin_lock_bh(&adapter->cmd_pending_queue_lock);

	if (list_empty(&adapter->cmd_pending_queue)) {
		/* No command to process */
		esp_verbose("No more command in queue.\n");
		spin_unlock_bh(&adapter->cmd_pending_queue_lock);
		spin_unlock_bh(&adapter->cmd_lock);
		return;
	}

	cmd_node = list_first_entry(&adapter->cmd_pending_queue,
				    struct command_node, list);
	if (!cmd_node) {
		esp_dbg("cmd node NULL\n");
		spin_unlock_bh(&adapter->cmd_pending_queue_lock);
		spin_unlock_bh(&adapter->cmd_lock);
		return;
	}
	esp_verbose("Processing Command [0x%X]\n", cmd_node->cmd_code);

	if (!esp_cmd_is_admitted(adapter, cmd_node->cmd_code)) {
		list_del_init(&cmd_node->list);
		cmd_node->in_pending_queue = false;
		cmd_node->result = -ESHUTDOWN;
		cmd_node->completed = true;
		spin_unlock_bh(&adapter->cmd_pending_queue_lock);
		spin_unlock_bh(&adapter->cmd_lock);
		wake_up_all(&adapter->wait_for_cmd_resp);
		esp_dbg("CMD_WORK_REJECT_SHUTDOWN code=%u seq=%u\n",
			cmd_node->cmd_code, cmd_node->cmd_seq);
		return;
	}

	list_del_init(&cmd_node->list);
	cmd_node->in_pending_queue = false;

	/* this should never happen */
	if (!cmd_node->cmd_skb || !cmd_node->cmd_code) {
		esp_warn("cmd_node->cmd_skb =%p , cmd_code=[0x%X]\n", cmd_node->cmd_skb, cmd_node->cmd_code);
		cmd_node->result = -EINVAL;
		cmd_node->completed = true;
		spin_unlock_bh(&adapter->cmd_pending_queue_lock);
		spin_unlock_bh(&adapter->cmd_lock);
		wake_up_all(&adapter->wait_for_cmd_resp);
		return;
	}

	/* Set as current cmd */
	adapter->cur_cmd = cmd_node;

	adapter->cmd_resp = 0;
	adapter->cmd_resp_seq = 0;

	payload_header = (struct esp_payload_header *)cmd_node->cmd_skb->data;
	if (adapter->capabilities & ESP_CHECKSUM_ENABLED)
		payload_header->checksum = esp_wire_cpu_to_le16(
			compute_checksum(cmd_node->cmd_skb->data,
				esp_wire_le16_to_cpu(payload_header->len) +
				esp_wire_le16_to_cpu(payload_header->offset)));

	cmd_node->sent_at = jiffies;
	esp_dbg("CMD_TRANSPORT_SUBMIT code=%u seq=%u queued_ms=%u skb_len=%u\n",
			cmd_node->cmd_code, cmd_node->cmd_seq,
			jiffies_to_msecs(cmd_node->sent_at - cmd_node->queued_at),
			cmd_node->cmd_skb->len);

	ret = esp_send_packet(adapter, cmd_node->cmd_skb);
	cmd_node->cmd_skb = NULL;

	if (ret) {
		esp_err("CMD_TRANSPORT_SUBMIT_FAILED code=%u seq=%u ret=%d\n",
				cmd_node->cmd_code, cmd_node->cmd_seq, ret);
		cmd_node->result = ret;
		cmd_node->completed = true;
		spin_unlock_bh(&adapter->cmd_pending_queue_lock);
		spin_unlock_bh(&adapter->cmd_lock);
		wake_up_all(&adapter->wait_for_cmd_resp);
		return;
	}

	spin_unlock_bh(&adapter->cmd_pending_queue_lock);
	spin_unlock_bh(&adapter->cmd_lock);
}

static int create_cmd_wq(struct esp_adapter *adapter)
{
	adapter->cmd_wq = create_singlethread_workqueue("ESP_CMD_WORK_QUEUE");

	RET_ON_FAIL(!adapter->cmd_wq);

	INIT_WORK(&adapter->cmd_work, esp_cmd_work);

	return 0;
}

static void destroy_cmd_wq(struct esp_adapter *adapter)
{
	if (adapter->cmd_wq) {
		cancel_work_sync(&adapter->cmd_work);
		destroy_workqueue(adapter->cmd_wq);
		adapter->cmd_wq = NULL;
	}
	if (adapter->if_rx_workqueue) {
		flush_workqueue(adapter->if_rx_workqueue);
	}

}

static struct command_node *prepare_command_request(struct esp_adapter *adapter, u8 cmd_code, size_t len)
{
	struct command_header *cmd;
	struct esp_payload_header *payload_header;
	struct command_node *node = NULL;
	struct esp_wifi_device *priv;

	if (!adapter) {
		esp_info("%u null adapter\n", __LINE__);
		return ERR_PTR(-EINVAL);
	}

	priv = adapter->priv[0];
	if (!priv) {
		esp_err("No command interface context\n");
		return ERR_PTR(-ENODEV);
	}

	if (!cmd_code || cmd_code >= CMD_MAX) {
		esp_err("unsupported command code\n");
		return ERR_PTR(-EINVAL);
	}
	if (!test_bit(ESP_CMD_INIT_DONE, &adapter->state_flags)) {
		esp_dbg("command queue init is not done yet\n");
		return ERR_PTR(-EBUSY);
	}
	if (!esp_cmd_is_admitted(adapter, cmd_code)) {
		esp_dbg("Rejecting command %u: not admitted\n", cmd_code);
		return ERR_PTR(-EBUSY);
	}

	if (test_bit(ESP_OTA_IN_PROGRESS, &adapter->state_flags)) {
		if (cmd_code != CMD_START_OTA_UPDATE &&
			cmd_code != CMD_START_OTA_WRITE &&
			cmd_code != CMD_START_OTA_END) {
			esp_dbg("OTA in progress discarding command %u\n", cmd_code);
			return ERR_PTR(-EBUSY);
		}
	}

	node = get_free_cmd_node(adapter, cmd_code);

	if (IS_ERR(node))
		return node;
	if (!node || !node->cmd_skb) {
		esp_err("Failed to get new free cmd node\n");
		return ERR_PTR(-ENOMEM);
	}

	node->cmd_code = cmd_code;
	spin_lock_bh(&adapter->cmd_lock);
	node->cmd_seq = ++adapter->next_cmd_seq;
	if (!node->cmd_seq)
		node->cmd_seq = ++adapter->next_cmd_seq;
	spin_unlock_bh(&adapter->cmd_lock);
	node->completed = false;
	node->result = 0;

	if (esp_size_add_overflow(len, sizeof(struct esp_payload_header), &len)) {
		esp_err("command overflow code=%u\n", cmd_code);
		recycle_cmd_node(adapter, node);
		return ERR_PTR(-EOVERFLOW);
	}
	if (len > ESP_SIZE_OF_CMD_NODE ||
	    len > (size_t)skb_tailroom(node->cmd_skb)) {
		esp_err("command too large code=%u total=%zu cap=%u tailroom=%u\n",
			cmd_code, len, ESP_SIZE_OF_CMD_NODE,
			skb_tailroom(node->cmd_skb));
		recycle_cmd_node(adapter, node);
		return ERR_PTR(-EINVAL);
	}

	payload_header = (struct esp_payload_header *)skb_put(node->cmd_skb, len);
	memset(payload_header, 0, len);

	payload_header->if_type = priv->if_type;
	payload_header->len = esp_wire_cpu_to_le16(
		len - sizeof(struct esp_payload_header));
	payload_header->offset = esp_wire_cpu_to_le16(
		sizeof(struct esp_payload_header));
	payload_header->packet_type = PACKET_TYPE_COMMAND_REQUEST;

	cmd = (struct command_header *)(node->cmd_skb->data +
					       sizeof(struct esp_payload_header));
	cmd->cmd_code = cmd_code;
	cmd->seq_num = esp_wire_cpu_to_le16(node->cmd_seq);

/*	payload_header->checksum = cpu_to_le16(compute_checksum(skb->data, len));*/
	return node;
}

static int cmd_prepare_err(struct command_node *node)
{
	if (IS_ERR(node))
		return PTR_ERR(node);
	esp_err("Failed to get command node\n");
	return -ENOMEM;
}

int process_cmd_resp(struct esp_adapter *adapter, struct sk_buff *skb)
{
	struct command_header *header;
	u16 resp_seq;

	if (!skb || !adapter) {
		esp_err("CMD resp: invalid!\n");

		if (skb)
			dev_kfree_skb_any(skb);

		return -1;
	}

	if (!test_bit(ESP_CMD_INIT_DONE, &adapter->state_flags)) {
		esp_err("CMD resp: cmd init is not done yet\n");
		if (skb)
			dev_kfree_skb_any(skb);
		return -1;
	}
	if (skb->len < sizeof(*header)) {
		esp_err("CMD_RESP_RUNT skb_len=%u expected=%zu\n",
				skb->len, sizeof(*header));
		dev_kfree_skb_any(skb);
		return -EINVAL;
	}

	header = (struct command_header *)skb->data;
	resp_seq = esp_wire_le16_to_cpu(header->seq_num);
	if (esp_wire_le16_to_cpu(header->len) > skb->len - sizeof(*header)) {
		esp_err("CMD_RESP_BAD_LEN code=%u seq=%u data_len=%u skb_len=%u\n",
				header->cmd_code, resp_seq,
				esp_wire_le16_to_cpu(header->len), skb->len);
		dev_kfree_skb_any(skb);
		return -EMSGSIZE;
	}

	/* Firmware TX-power values are wire little-endian. Convert the value in
	 * place before the unchanged legacy decoder assigns it to priv->tx_pwr. */
	if ((header->cmd_code == CMD_GET_TXPOWER ||
	     header->cmd_code == CMD_SET_TXPOWER) &&
	    skb->len >= sizeof(struct cmd_set_get_val) &&
	    esp_wire_le16_to_cpu(header->len) >= sizeof(u32)) {
		struct cmd_set_get_val *value = (struct cmd_set_get_val *)skb->data;

		value->value = esp_wire_le32_to_cpu(value->value);
	}

	/* Both frozen and corrected firmware transmit the MGMT frame length from
	 * the little-endian ESP. Normalize it before the unchanged legacy response
	 * decoder inspects it; zero-length negotiated acceptance is unchanged. */
	if (header->cmd_code == CMD_MGMT_TX &&
	    skb->len >= sizeof(struct cmd_mgmt_tx)) {
		struct cmd_mgmt_tx *mgmt = (struct cmd_mgmt_tx *)skb->data;

		mgmt->len = esp_wire_le32_to_cpu(mgmt->len);
	}

	spin_lock_bh(&adapter->cmd_lock);
	if (!adapter->cur_cmd) {
		esp_err("CMD_RESP_UNEXPECTED code=%u seq=%u status=%u len=%u\n",
				header->cmd_code, resp_seq, header->cmd_status,
				esp_wire_le16_to_cpu(header->len));
		dev_kfree_skb_any(skb);
		spin_unlock_bh(&adapter->cmd_lock);
		return -1;
	}
	if (header->cmd_code != adapter->cur_cmd->cmd_code ||
			resp_seq != adapter->cur_cmd->cmd_seq) {
		esp_err("CMD_RESP_MISMATCH expected=%u/%u got=%u/%u status=%u skb_len=%u\n",
				adapter->cur_cmd->cmd_code, adapter->cur_cmd->cmd_seq,
				header->cmd_code, resp_seq, header->cmd_status, skb->len);
		dev_kfree_skb_any(skb);
		spin_unlock_bh(&adapter->cmd_lock);
		return -EPROTO;
	}
	if (adapter->cur_cmd->completed || adapter->cur_cmd->resp_skb) {
		esp_err("CMD_RESP_DUPLICATE code=%u seq=%u\n",
				header->cmd_code, resp_seq);
		dev_kfree_skb_any(skb);
		spin_unlock_bh(&adapter->cmd_lock);
		return -EALREADY;
	}

	/* Only a successful response from new firmware marks submission
	 * acceptance. Suppress the legacy response-as-radio-status path only then.
	 * Old firmware leaves reserved1 zero and keeps deferred-completion behavior. */
	if (header->cmd_code == CMD_MGMT_TX &&
	    header->cmd_status == CMD_RESPONSE_SUCCESS &&
	    header->reserved1 == HOSTED_MGMT_TX_ASYNC_STATUS_V1)
		adapter->cur_cmd->mgmt_dont_wait_for_ack = true;

	adapter->cur_cmd->resp_skb = skb;
	adapter->cur_cmd->result = 0;
	adapter->cur_cmd->completed = true;
	adapter->cmd_resp = header->cmd_code;
	adapter->cmd_resp_seq = resp_seq;
	esp_dbg("CMD_RESP_MATCH code=%u seq=%u status=%u elapsed_ms=%u\n",
			header->cmd_code, resp_seq, header->cmd_status,
			adapter->cur_cmd->sent_at ?
			jiffies_to_msecs(jiffies - adapter->cur_cmd->sent_at) : 0);
	spin_unlock_bh(&adapter->cmd_lock);

	wake_up_all(&adapter->wait_for_cmd_resp);

	return 0;
}

void esp_cmd_transport_failed(struct esp_adapter *adapter, uint8_t cmd_code,
		u16 cmd_seq, int error)
{
	if (!adapter)
		return;

	spin_lock_bh(&adapter->cmd_lock);
	if (!adapter->cur_cmd || adapter->cur_cmd->cmd_code != cmd_code ||
			adapter->cur_cmd->cmd_seq != cmd_seq) {
		esp_err("CMD_TRANSPORT_FAILURE_STALE code=%u seq=%u error=%d current=%u/%u\n",
				cmd_code, cmd_seq, error,
				adapter->cur_cmd ? adapter->cur_cmd->cmd_code : 0,
				adapter->cur_cmd ? adapter->cur_cmd->cmd_seq : 0);
		spin_unlock_bh(&adapter->cmd_lock);
		return;
	}

	if (adapter->cur_cmd->completed || adapter->cur_cmd->resp_skb) {
		esp_dbg("CMD_TRANSPORT_FAILURE_AFTER_COMPLETE code=%u seq=%u error=%d\n",
			cmd_code, cmd_seq, error);
		spin_unlock_bh(&adapter->cmd_lock);
		return;
	}

	adapter->cur_cmd->result = error ? error : -EIO;
	adapter->cur_cmd->completed = true;
	/* The waiting command caller owns cur_cmd teardown, node recycling, and
	 * scheduling the next command. Keep cur_cmd set until that caller wakes. */
	esp_err("CMD_TRANSPORT_FAILURE code=%u seq=%u error=%d\n",
			cmd_code, cmd_seq, adapter->cur_cmd->result);
	spin_unlock_bh(&adapter->cmd_lock);
	wake_up_all(&adapter->wait_for_cmd_resp);
}

static void process_mgmt_tx_status(struct esp_wifi_device *priv,
				   int ack, uint8_t *data, uint32_t len, u64 cookie);
static int handle_mgmt_tx_done(struct esp_wifi_device *priv,
				struct command_node *cmd_node)
{
	struct cmd_mgmt_tx *resp;
	u8 *frame = NULL;
	u32 frame_len = 0;
	bool ack;

	if (!priv || !cmd_node ||
	    !cmd_node->resp_skb ||
	    !cmd_node->resp_skb->data ||
	    cmd_node->resp_skb->len < sizeof(struct cmd_mgmt_tx)) {
		esp_err("invalid arg or truncated mgmt tx response\n");
		return -EINVAL;
	}

	resp = (struct cmd_mgmt_tx *) (cmd_node->resp_skb->data);

	/* Rejected before transmit: cfg80211 does not expect a TX-status. */
	if (resp->header.cmd_status == CMD_RESPONSE_BUSY)
		return -EBUSY;
	if (resp->header.cmd_status == CMD_RESPONSE_INVALID ||
	    resp->header.cmd_status == CMD_RESPONSE_UNSUPPORTED)
		return -EINVAL;

	if (cmd_node->mgmt_dont_wait_for_ack)
		return 0;

	/* cmd_skb is given to transport on submit. Keep a copy of the original
	 * request frame on the command node for broadcast / empty completions. */
	if (cmd_node->mgmt_frame && cmd_node->mgmt_frame_len) {
		frame = cmd_node->mgmt_frame;
		frame_len = cmd_node->mgmt_frame_len;
	}

	/* Unicast completion may echo the on-air frame; broadcast uses len=0. */
	if (resp->len &&
	    resp->len <= cmd_node->resp_skb->len - sizeof(struct cmd_mgmt_tx)) {
		frame = resp->buf;
		frame_len = resp->len;
	}

	ack = (resp->header.cmd_status == CMD_RESPONSE_SUCCESS);
	process_mgmt_tx_status(priv, ack, frame, frame_len, cmd_node->cookie);
	return 0;
}

static bool skb_has_flex_frame(const struct sk_buff *skb, size_t prefix, u32 frame_len)
{
	if (!skb || skb->len < prefix)
		return false;
	return frame_len <= skb->len - prefix;
}

static void process_scan_result_event(struct esp_wifi_device *priv,
		struct sk_buff *skb)
{
	struct scan_event *scan_evt;
	struct cfg80211_bss *bss = NULL;
	struct beacon_probe_fixed_params *fixed_params = NULL;
	struct ieee80211_channel *chan = NULL;
	u8 *ie_buf = NULL;
	u64 timestamp;
	u16 beacon_interval;
	u16 cap_info;
	u16 frame_len;
	u32 ie_len;
	int freq;
	int frame_type = CFG80211_BSS_FTYPE_UNKNOWN; /* int type for older compatibility */

	if (!priv || !skb) {
		esp_err("Invalid arguments\n");
		return;
	}
	/* Scan-done is only struct event_header (status=0). Probe/beacon
	 * results are a full scan_event. Require the short header first. */
	if (skb->len < sizeof(struct event_header)) {
		esp_err("SCAN_EVENT truncated skb_len=%u\n", skb->len);
		return;
	}

	scan_evt = (struct scan_event *)skb->data;

	/* End of scan; notify cfg80211 */
	if (scan_evt->header.status == 0) {
		if (!priv->scan_in_progress && !priv->request &&
				!priv->waiting_for_scan_done) {
			esp_warn("SCAN_DONE_UNEXPECTED ignored\n");
			return;
		}

		esp_dbg("SCAN_DONE_RX request=%p blocking=%u\n",
				priv->request, priv->waiting_for_scan_done);
		cancel_delayed_work(&priv->scan_timeout_work);
		ESP_MARK_SCAN_DONE(priv, false);
		if (priv->waiting_for_scan_done) {
			priv->waiting_for_scan_done = false;
			wake_up_interruptible(&priv->wait_for_scan_completion);
		}
		return;
	}
	if (skb->len < offsetof(struct scan_event, frame)) {
		esp_err("SCAN_EVENT truncated skb_len=%u prefix=%zu\n",
			skb->len, offsetof(struct scan_event, frame));
		return;
	}
	if (!priv->scan_in_progress) {
		esp_dbg("SCAN_RESULT_STALE ignored status=%u len=%u\n",
				scan_evt->header.status,
				esp_wire_le16_to_cpu(scan_evt->header.len));
		return;
	}

	frame_len = esp_wire_le16_to_cpu(scan_evt->frame_len);
	if (!skb_has_flex_frame(skb, offsetof(struct scan_event, frame), frame_len) ||
	    frame_len < sizeof(struct beacon_probe_fixed_params)) {
		esp_err("SCAN_EVENT bad frame_len=%u skb_len=%u\n",
			frame_len, skb->len);
		return;
	}

	ie_buf = (u8 *) scan_evt->frame;
	ie_len = frame_len;

	fixed_params = (struct beacon_probe_fixed_params *) ie_buf;

	timestamp = le64_to_cpu(fixed_params->timestamp);
	beacon_interval = le16_to_cpu(fixed_params->beacon_interval);
	cap_info = le16_to_cpu(fixed_params->cap_info);

	if (scan_evt->channel > 14) {
		freq = ieee80211_channel_to_frequency(scan_evt->channel, NL80211_BAND_5GHZ);
	} else {
		freq = ieee80211_channel_to_frequency(scan_evt->channel, NL80211_BAND_2GHZ);
	}
	chan = ieee80211_get_channel(priv->adapter->wiphy, freq);

	ie_buf += sizeof(struct beacon_probe_fixed_params);
	ie_len -= sizeof(struct beacon_probe_fixed_params);

	if ((scan_evt->frame_type << 4) == IEEE80211_STYPE_BEACON) {
		frame_type = CFG80211_BSS_FTYPE_BEACON;
	} else if ((scan_evt->frame_type << 4) == IEEE80211_STYPE_PROBE_RESP) {
		frame_type = CFG80211_BSS_FTYPE_PRESP;
	}

	if (chan && !(chan->flags & IEEE80211_CHAN_DISABLED)) {
		bss = CFG80211_INFORM_BSS(priv->adapter->wiphy, chan,
				frame_type, scan_evt->bssid, timestamp,
				cap_info, beacon_interval, ie_buf, ie_len,
				(esp_wire_le32_to_cpu(scan_evt->rssi) * 100), GFP_ATOMIC);

		if (bss)
			cfg80211_put_bss(priv->adapter->wiphy, bss);
	} else {
		esp_info("Scan report: Skip invalid or disabled channel\n");
	}
}

static void process_auth_event(struct esp_wifi_device *priv,
		struct sk_buff *skb)
{
	struct auth_event *event;
	u16 frame_len;
	bool busy;
	bool match;

	if (!priv || !skb || !priv->ndev) {
		esp_err("Invalid arguments\n");
		return;
	}
	if (skb->len < offsetof(struct auth_event, frame)) {
		esp_err("AUTH_EVENT truncated skb_len=%u\n", skb->len);
		return;
	}

	event = (struct auth_event *)skb->data;
	frame_len = esp_wire_le16_to_cpu(event->frame_len);
	if (!skb_has_flex_frame(skb, offsetof(struct auth_event, frame), frame_len)) {
		esp_err("AUTH_EVENT bad frame_len=%u skb_len=%u\n",
			frame_len, skb->len);
		return;
	}

	spin_lock_bh(&priv->bss_lock);
	match = (priv->auth_cmd_pending || priv->auth_awaiting_mlme) &&
		priv->auth_cmd_seq &&
		esp_wire_le16_to_cpu(event->auth_seq) == priv->auth_cmd_seq &&
		!memcmp(priv->auth_bssid, event->bssid, MAC_ADDR_LEN);
	busy = esp_mlme_busy(priv);
	spin_unlock_bh(&priv->bss_lock);

	if (!match) {
		esp_info("Drop AUTH_RX for unexpected BSSID/generation\n");
		return;
	}

	esp_mlme_stage_event(priv, skb, EVENT_AUTH_RX);
	if (!busy && priv->adapter && priv->adapter->events_wq)
		queue_work(priv->adapter->events_wq, &priv->mlme_work);
}

static int chan_to_freq(u8 chan)
{
	if (chan >= 1 && chan <= 13) {
		return (2407 + 5 * chan);
	} else if (chan == 14) {
		return 2484;
	} else if (chan >= 32 && chan <= 177) {
		return (5000 + 5 * chan);
	} else {
		return -1;
	}
}

static void process_mgmt_tx_status(struct esp_wifi_device *priv,
				   int ack, uint8_t *data, uint32_t len, u64 cookie)
{
	if (!priv || !priv->ndev)
		return;

	cfg80211_mgmt_tx_status(&priv->wdev, cookie, data, len,
				ack, GFP_ATOMIC);
}

static void process_ap_mgmt_rx(struct esp_wifi_device *priv, struct sk_buff *skb)
{
	struct mgmt_event *event;
	u32 frame_len;

	if (!priv || !skb || !priv->ndev)
		return;
	if (skb->len < offsetof(struct mgmt_event, frame)) {
		esp_err("MGMT_EVENT truncated skb_len=%u\n", skb->len);
		return;
	}

	event = (struct mgmt_event *)skb->data;
	frame_len = esp_wire_le32_to_cpu(event->frame_len);
	if (!skb_has_flex_frame(skb, offsetof(struct mgmt_event, frame), frame_len)) {
		esp_err("MGMT_EVENT bad frame_len=%u skb_len=%u\n",
			frame_len, skb->len);
		return;
	}

	cfg80211_rx_mgmt(&priv->wdev, chan_to_freq(event->chan),
		event->rssi, event->frame, frame_len, 0);
}

static void process_disconnect_event(struct esp_wifi_device *priv,
		struct sk_buff *skb)
{
	struct disconnect_event *event;
	bool busy;
	bool match = false;
	u16 disc_seq;

	if (!priv || !skb) {
		esp_err("Invalid arguments\n");
		return;
	}
	if (skb->len < sizeof(struct disconnect_event)) {
		esp_err("DISCONNECT_EVENT truncated skb_len=%u\n", skb->len);
		return;
	}

	event = (struct disconnect_event *)skb->data;
	disc_seq = esp_wire_le16_to_cpu(event->disconnect_seq);

	spin_lock_bh(&priv->bss_lock);
	if ((priv->disconnect_cmd_pending || priv->disconnect_awaiting_mlme) &&
	    priv->disconnect_cmd_seq && disc_seq == priv->disconnect_cmd_seq) {
		match = true;
	} else if ((priv->conn_generation && disc_seq == priv->conn_generation) ||
		   (priv->staged_assoc_seq && disc_seq == priv->staged_assoc_seq)) {
		match = true;
	} else if (priv->local_disconnect_req &&
		   (priv->disconnect_cmd_pending || priv->disconnect_awaiting_mlme) &&
		   !priv->disconnect_cmd_seq) {
		match = true;
	}
	busy = esp_mlme_busy(priv);
	spin_unlock_bh(&priv->bss_lock);

	if (!match) {
		esp_info("Drop DISCONNECT_EVENT for unexpected generation (seq=%u local=%u conn=%u)\n",
			 disc_seq, priv->disconnect_cmd_seq, priv->conn_generation);
		return;
	}

	esp_mlme_stage_event(priv, skb, EVENT_STA_DISCONNECT);
	if (!busy && priv->adapter && priv->adapter->events_wq)
		queue_work(priv->adapter->events_wq, &priv->mlme_work);
}

static void process_assoc_event(struct esp_wifi_device *priv,
		struct sk_buff *skb)
{
	struct assoc_event *event;
	u16 frame_len;
	bool match;
	bool busy;

	if (!priv || !skb) {
		esp_err("Invalid arguments\n");
		return;
	}
	if (skb->len < offsetof(struct assoc_event, frame)) {
		esp_err("ASSOC_EVENT truncated skb_len=%u\n", skb->len);
		return;
	}

	event = (struct assoc_event *)skb->data;
	frame_len = esp_wire_le16_to_cpu(event->frame_len);
	if (!skb_has_flex_frame(skb, offsetof(struct assoc_event, frame), frame_len)) {
		esp_err("ASSOC_EVENT bad frame_len=%u skb_len=%u\n",
			frame_len, skb->len);
		return;
	}

	esp_info("Connection status: %d\n", event->header.status);

	spin_lock_bh(&priv->bss_lock);
	match = (priv->assoc_cmd_pending || priv->assoc_awaiting_mlme) &&
		priv->assoc_cmd_seq &&
		esp_wire_le16_to_cpu(event->assoc_seq) == priv->assoc_cmd_seq &&
		!memcmp(priv->assoc_bssid, event->bssid, MAC_ADDR_LEN);
	busy = esp_mlme_busy(priv);
	spin_unlock_bh(&priv->bss_lock);

	if (!match) {
		esp_info("Drop ASSOC_RX for unexpected BSSID/generation\n");
		return;
	}

	esp_mlme_stage_event(priv, skb, EVENT_ASSOC_RX);
	if (!busy && priv->adapter && priv->adapter->events_wq)
		queue_work(priv->adapter->events_wq, &priv->mlme_work);
}

int process_cmd_event(struct esp_wifi_device *priv, struct sk_buff *skb)
{
	struct event_header *header;

	if (!skb || !priv || !skb->data) {
		esp_err("CMD evnt: invalid!\n");
		return -1;
	}
	if (skb->len < sizeof(*header)) {
		esp_err("CMD event truncated skb_len=%u\n", skb->len);
		return -EMSGSIZE;
	}

	header = (struct event_header *) (skb->data);

	switch (header->event_code) {

	case EVENT_SCAN_RESULT:
		process_scan_result_event(priv, skb);
		break;

	case EVENT_ASSOC_RX:
		process_assoc_event(priv, skb);
		break;

	case EVENT_STA_DISCONNECT:
		process_disconnect_event(priv, skb);
		break;

	case EVENT_AUTH_RX:
		process_auth_event(priv, skb);
		break;

	case EVENT_AP_MGMT_RX:
		process_ap_mgmt_rx(priv, skb);
		break;

	case HOSTED_EVENT_MGMT_TX_STATUS: {
		struct hosted_mgmt_tx_status_event *event;
		u32 frame_len;
		u64 tx_id;
		u64 cookie = 0;
		bool valid = false;

		if (skb->len < offsetof(struct hosted_mgmt_tx_status_event, frame))
			return -EINVAL;
		event = (struct hosted_mgmt_tx_status_event *)skb->data;
		frame_len = esp_wire_le32_to_cpu(event->frame_len);
		tx_id = esp_wire_le64_to_cpu(event->cookie);
		if (!tx_id || !skb_has_flex_frame(skb,
				offsetof(struct hosted_mgmt_tx_status_event, frame),
				frame_len))
			return -EINVAL;

		spin_lock_bh(&priv->bss_lock);
		if (priv->pending_mgmt_active && priv->pending_mgmt_id == tx_id) {
			cookie = priv->pending_mgmt_cookie;
			priv->pending_mgmt_active = false;
			valid = true;
		}
		spin_unlock_bh(&priv->bss_lock);

		if (!valid) {
			esp_info("Drop MGMT_TX_STATUS for stale/mismatched id=0x%llx\n", tx_id);
			break;
		}

		process_mgmt_tx_status(priv, !!event->ack, event->frame,
				       frame_len, cookie);
		break;
	}

	default:
		esp_info("%u unhandled event[%u]\n",
				__LINE__, header->event_code);
		break;
	}

	return 0;
}

int cmd_set_mcast_mac_list(struct esp_wifi_device *priv, struct multicast_list *list)
{
	struct command_node *cmd_node = NULL;
	struct cmd_set_mcast_mac_addr *cmd_mcast_mac_list;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags))
		return 0;

	cmd_node = prepare_command_request(priv->adapter, CMD_SET_MCAST_MAC_ADDR,
			sizeof(struct cmd_set_mcast_mac_addr));

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd_mcast_mac_list = (struct cmd_set_mcast_mac_addr *)
		(cmd_node->cmd_skb->data + sizeof(struct esp_payload_header));

	cmd_mcast_mac_list->count = list->addr_count;
	memcpy(cmd_mcast_mac_list->mcast_addr, list->mcast_addr,
			sizeof(cmd_mcast_mac_list->mcast_addr));

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));
	return 0;
}

int cmd_set_ip_address(struct esp_wifi_device *priv, __be32 ip)
{
	struct command_node *cmd_node = NULL;
	struct cmd_set_ip_addr *cmd_set_ip;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags))
		return 0;

	cmd_node = prepare_command_request(priv->adapter, CMD_SET_IP_ADDR,
			sizeof(struct cmd_set_ip_addr));

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd_set_ip = (struct cmd_set_ip_addr *)
		(cmd_node->cmd_skb->data + sizeof(struct esp_payload_header));

	/* This protocol field carries the IPv4 address byte sequence unchanged.
	 * Copy the __be32 object representation instead of converting it to host
	 * order, preserving the existing wire format on every host endianness.
	 */
	memcpy(&cmd_set_ip->ip, &ip, sizeof(ip));

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_disconnect_request(struct esp_wifi_device *priv, u16 reason_code,
			   const uint8_t *mac, u8 subtype)
{
	struct command_node *cmd_node = NULL;
	struct cmd_disconnect *cmd;
	int ret;

	if (!priv || !priv->adapter)
		return -EINVAL;
	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags))
		return 0;

	cmd_node = prepare_command_request(priv->adapter, CMD_DISCONNECT,
					   sizeof(struct cmd_disconnect));
	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	esp_wifi_clear_assoc_pending(priv);
	esp_wifi_clear_auth_pending(priv);

	cmd = (struct cmd_disconnect *)(cmd_node->cmd_skb->data +
					 sizeof(struct esp_payload_header));
	cmd->header.reserved1 = subtype;
	cmd->reason_code = esp_wire_cpu_to_le16(reason_code);
	if (mac)
		memcpy(cmd->mac, mac, ETH_ALEN);

	spin_lock_bh(&priv->bss_lock);
	priv->local_disconnect_req = true;
	priv->disconnect_cmd_seq = cmd_node->cmd_seq;
	if (mac)
		memcpy(priv->disconnect_bssid, mac, ETH_ALEN);
	else
		memset(priv->disconnect_bssid, 0, ETH_ALEN);
	spin_unlock_bh(&priv->bss_lock);

	esp_mlme_begin(priv, ESP_MLME_DISCONNECT);
	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);
	ret = wait_and_decode_cmd_resp(priv, cmd_node);

	/* decode_disconnect_resp() in the frozen body sets the local flag even
	 * for a failed firmware response. Clear it before staged MLME events are
	 * flushed so a failed request can never label a remote event as local. */
	if (ret || priv->if_type != ESP_STA_IF) {
		spin_lock_bh(&priv->bss_lock);
		priv->local_disconnect_req = false;
		spin_unlock_bh(&priv->bss_lock);
	}

	esp_mlme_end(priv, ESP_MLME_DISCONNECT, ret == 0);

	/* For synchronous disassoc, cfg80211 requires that upon return from
	 * .disassoc(), disconnection is already reported and wdev->connected is cleared. */
	if (!ret && priv->if_type == ESP_STA_IF && subtype == DISCONNECT_TYPE_DISASSOC) {
		spin_lock_bh(&priv->bss_lock);
		priv->local_disconnect_req = false;
		priv->disconnect_awaiting_mlme = false;
		priv->disconnect_cmd_seq = 0;
		priv->conn_generation = 0;
		memset(priv->disconnect_bssid, 0, MAC_ADDR_LEN);
		spin_unlock_bh(&priv->bss_lock);

		esp_wifi_put_bss(priv);
		esp_wifi_clear_assoc_pending(priv);
		esp_wifi_clear_auth_pending(priv);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0))
		if (priv->adapter)
			cfg80211_bss_flush(priv->adapter->wiphy);
#endif
		esp_port_close(priv);
		CFG80211_DISCONNECTED(priv->ndev, reason_code, NULL, 0, true, GFP_KERNEL);
	} else if (!ret && priv->if_type == ESP_STA_IF) {
		spin_lock_bh(&priv->bss_lock);
		if (!priv->local_disconnect_req)
			priv->disconnect_awaiting_mlme = false;
		spin_unlock_bh(&priv->bss_lock);
	}
	return ret;
}

#if 0
int cmd_connect_request(struct esp_wifi_device *priv,
		struct cfg80211_connect_params *params)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;
	struct cmd_sta_connect *cmd;
	struct ieee80211_channel *chan;
	struct cfg80211_bss *bss;
	struct esp_adapter *adapter = NULL;
	u8 retry = 2;

	if (!priv || !params || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags)) {
		esp_err("%u cleanup in progress, return failure", __LINE__);
		return -EFAULT;
	}

	adapter = priv->adapter;

	cmd_len = sizeof(struct cmd_sta_connect) + params->ie_len;

	cmd_node = prepare_command_request(adapter, CMD_STA_CONNECT, cmd_len);
	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);
	cmd = (struct cmd_sta_connect *) (cmd_node->cmd_skb->data + sizeof(struct esp_payload_header));

	if (params->ssid_len)
		memcpy(cmd->ssid, params->ssid, MAX_SSID_LEN);
	else
		esp_err("No ssid\n");

	if (params->bssid) {
		memcpy(cmd->bssid, params->bssid, MAC_ADDR_LEN);
	}

	if (params->channel) {
		chan = params->channel;
		cmd->channel = chan->hw_value;
	}

	if (params->ie_len) {
		cmd->assoc_ie_len = cpu_to_le16(params->ie_len);
		memcpy(cmd->assoc_ie, params->ie, params->ie_len);
	}

	if (params->privacy)
		cmd->is_auth_open = 0;
	else
		cmd->is_auth_open = 1;

	esp_info("Connection request: %s "MACSTR" %d\n",
			cmd->ssid, MAC2STR(params->bssid), cmd->channel);

	do {
		bss = cfg80211_get_bss(adapter->wiphy, params->channel, params->bssid,
				params->ssid, params->ssid_len, IEEE80211_BSS_TYPE_ESS, IEEE80211_PRIVACY_ANY);

		if (bss) {
			break;
		} else {
			esp_info("No BSS in the list.. scanning..\n");
			internal_scan_request(priv, cmd->ssid, cmd->channel, true);
		}

		retry--;
	} while (retry);

	if (retry) {
		queue_cmd_node(adapter, cmd_node, ESP_CMD_DFLT_PRIO);
		queue_work(adapter->cmd_wq, &adapter->cmd_work);

		RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));
	} else {
		esp_info("Failed to find %s\n", cmd->ssid);
		return -EFAULT;
	}

	return 0;
}
#endif


int cmd_assoc_request(struct esp_wifi_device *priv,
		struct cfg80211_assoc_request *req)
{
	struct command_node *cmd_node = NULL;
	struct cmd_sta_assoc *cmd;
	struct cfg80211_bss *bss;
	struct esp_adapter *adapter = NULL;
	size_t cmd_len;
	int ret;
	u8 *new_ie, *old_ie;
	bool need_bss_ref;

	if (!priv || !req || !req->bss || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (priv->if_type != ESP_STA_IF) {
		esp_err("Invalid interface\n");
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags)) {
		esp_err("%u cleanup in progress, return failure", __LINE__);
		return -EFAULT;
	}

	if (req->ie_len > U8_MAX || (req->ie_len && !req->ie)) {
		esp_err("Association IE too large or missing: %zu\n", req->ie_len);
		return -EINVAL;
	}

	bss = req->bss;
	adapter = priv->adapter;

	if (esp_size_add_overflow(sizeof(struct cmd_sta_assoc), req->ie_len, &cmd_len))
		return -EOVERFLOW;

	cmd_node = prepare_command_request(adapter, CMD_STA_ASSOC, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd = (struct cmd_sta_assoc *) (cmd_node->cmd_skb->data + sizeof(struct esp_payload_header));

	cmd->assoc_ie_len = req->ie_len;
	cmd->is_reassoc = req->prev_bssid ? 1 : 0;
	cmd->control_port = req->crypto.control_port ? 1 : 0;
	memset(cmd->pad, 0, sizeof(cmd->pad));
	memcpy(cmd->assoc_ie, req->ie, req->ie_len);

	new_ie = NULL;
	if (req->ie_len) {
		new_ie = kmemdup(req->ie, req->ie_len, GFP_KERNEL);
		if (!new_ie) {
			esp_err("Failed to allocate buffer for assoc request IEs\n");
			recycle_cmd_node(adapter, cmd_node);
			return -ENOMEM;
		}
	}

	spin_lock_bh(&priv->bss_lock);
	if (priv->assoc_cmd_pending || priv->assoc_awaiting_mlme) {
		spin_unlock_bh(&priv->bss_lock);
		kfree(new_ie);
		recycle_cmd_node(adapter, cmd_node);
		return -EBUSY;
	}
	old_ie = priv->assoc_req_ie;
	priv->assoc_req_ie = new_ie;
	priv->assoc_req_ie_len = req->ie_len;
	memcpy(priv->assoc_bssid, bss->bssid, MAC_ADDR_LEN);
	priv->assoc_cmd_seq = cmd_node->cmd_seq;
	priv->assoc_control_port = req->crypto.control_port;
	priv->assoc_cmd_pending = true;
	need_bss_ref = (priv->bss == NULL);
	spin_unlock_bh(&priv->bss_lock);
	kfree(old_ie);

	/* EVENT_ASSOC_RX can arrive during the command wait. Reassoc without
	 * a new .auth has no stored BSS; hold a ref so the event is not
	 * delivered with a NULL BSS. On command failure cfg80211 still owns
	 * req->bss, so drop only the ref we added. */
	if (need_bss_ref)
		esp_wifi_ref_bss(priv, req->bss);

	esp_mlme_begin(priv, ESP_MLME_ASSOC);

	esp_info("Association request: "MACSTR" %d %d\n",
			MAC2STR(bss->bssid), bss->channel->hw_value, cmd->assoc_ie_len);

	queue_cmd_node(adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	ret = wait_and_decode_cmd_resp(priv, cmd_node);
	if (ret) {
		esp_mlme_end(priv, ESP_MLME_ASSOC, false);
		esp_wifi_clear_assoc_pending(priv);
		if (need_bss_ref)
			esp_wifi_put_bss(priv);
		return ret;
	}

	esp_mlme_end(priv, ESP_MLME_ASSOC, true);

	/* cfg80211 transferred req->bss on a successful .assoc return. */
	esp_wifi_take_assoc_bss(priv, req->bss);

	return 0;
}

int cmd_sta_set_authorized(struct esp_wifi_device *priv, const u8 *bssid,
		bool authorized)
{
	struct command_node *cmd_node;
	struct cmd_sta_set_authorized *cmd;

	if (!priv || !priv->adapter || !bssid) {
		esp_err("STA_PORT_AUTH invalid argument\n");
		return -EINVAL;
	}

	if (priv->if_type != ESP_STA_IF) {
		esp_err("STA_PORT_AUTH invalid interface=%u\n", priv->if_type);
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags)) {
		esp_err("STA_PORT_AUTH cleanup in progress\n");
		return -ESHUTDOWN;
	}

	cmd_node = prepare_command_request(priv->adapter,
			CMD_STA_SET_AUTHORIZED, sizeof(*cmd));
	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd = (struct cmd_sta_set_authorized *)(cmd_node->cmd_skb->data +
			sizeof(struct esp_payload_header));
	memcpy(cmd->bssid, bssid, MAC_ADDR_LEN);
	cmd->authorized = authorized ? 1 : 0;

	esp_dbg("STA_PORT_AUTH_TX "MACSTR" authorized=%u\n",
			MAC2STR(cmd->bssid), cmd->authorized);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	if (authorized)
		esp_port_open(priv);
	else
		esp_port_close(priv);

	return 0;
}

int cmd_auth_request(struct esp_wifi_device *priv,
		struct cfg80211_auth_request *req)
{
	struct command_node *cmd_node = NULL;
	struct cmd_sta_auth *cmd;
	struct cfg80211_bss *bss;
	/*struct cfg80211_bss *bss1;*/
	const u8 *ssid_eid;
	u8 ssid[MAX_SSID_LEN];
	uint8_t ssid_len;
	struct esp_adapter *adapter = NULL;
	size_t cmd_len;
	size_t auth_data_len;
	size_t needed;
	const struct cfg80211_bss_ies *ies;
	int ret;
	/* u8 retry = 2; */

	if (!priv || !req || !req->bss || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (priv->if_type != ESP_STA_IF) {
		esp_err("Invalid interface\n");
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags)) {
		esp_err(":%u cleanup in progress, return failure", __LINE__);
		return -EFAULT;
	}

	bss = req->bss;

	rcu_read_lock();
	ies = rcu_dereference(bss->proberesp_ies);
	if (!ies)
		ies = rcu_dereference(bss->beacon_ies);
	if (!ies)
		ies = rcu_dereference(bss->ies);

	if (!ies) {
		rcu_read_unlock();
		esp_err("No BSS IEs available for authentication\n");
		return -EINVAL;
	}

	ssid_eid = cfg80211_find_ie(WLAN_EID_SSID, ies->data, ies->len);
	if (!ssid_eid || ssid_eid[1] > MAX_SSID_LEN) {
		rcu_read_unlock();
		esp_err("Invalid SSID IE in BSS data\n");
		return -EINVAL;
	}

	ssid_len = ssid_eid[1];
	memcpy(ssid, ssid_eid + 2, ssid_len);
	rcu_read_unlock();

	adapter = priv->adapter;

	auth_data_len = ESP_CFG80211_AUTH_DATA_LEN(req);
	if (req->ie_len) {
		needed = (auth_data_len ? auth_data_len : 4) + req->ie_len;
		if (needed > U8_MAX) {
			esp_err("Auth data too large: %zu\n", needed);
			return -EINVAL;
		}
		auth_data_len = needed;
	}
	if (auth_data_len > U8_MAX) {
		esp_err("Auth data too large: %zu\n", auth_data_len);
		return -EINVAL;
	}
	if (req->key_len > sizeof(((struct cmd_sta_auth *)0)->key)) {
		esp_err("Auth key too large: %u max=%zu\n", req->key_len,
			sizeof(((struct cmd_sta_auth *)0)->key));
		return -EINVAL;
	}

	if (esp_size_add_overflow(sizeof(struct cmd_sta_auth), auth_data_len, &cmd_len))
		return -EOVERFLOW;

	cmd_node = prepare_command_request(adapter, CMD_STA_AUTH, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);
	cmd = (struct cmd_sta_auth *) (cmd_node->cmd_skb->data + sizeof(struct esp_payload_header));

#define WLAN_AUTH_OPEN 0
#define WLAN_AUTH_FT 2
#define WLAN_AUTH_SAE 3
	if (req->auth_type == NL80211_AUTHTYPE_FT) {
		cmd->auth_type = WLAN_AUTH_FT;
	} else if (req->auth_type == NL80211_AUTHTYPE_SAE) {
		cmd->auth_type = WLAN_AUTH_SAE;
	} else if (req->auth_type == NL80211_AUTHTYPE_OPEN_SYSTEM) {
		cmd->auth_type = WLAN_AUTH_OPEN;
	} else {
		cmd->auth_type = req->auth_type;
	}
	memcpy(cmd->ssid, ssid, ssid_len);
	memcpy(cmd->bssid, bss->bssid, MAC_ADDR_LEN);
	cmd->channel = bss->channel->hw_value;
	cmd->auth_data_len = auth_data_len;
	if (ESP_CFG80211_AUTH_DATA_LEN(req)) {
		memcpy(cmd->auth_data, ESP_CFG80211_AUTH_DATA(req),
		       ESP_CFG80211_AUTH_DATA_LEN(req));
		if (req->ie && req->ie_len) {
			memcpy(cmd->auth_data + ESP_CFG80211_AUTH_DATA_LEN(req),
			       req->ie, req->ie_len);
		}
	} else if (req->ie && req->ie_len) {
		memset(cmd->auth_data, 0, 4);
		memcpy(cmd->auth_data + 4, req->ie, req->ie_len);
	}

	if (req->key_len) {
		memcpy(cmd->key, req->key, req->key_len);
		cmd->key_len = req->key_len;
	}
	esp_info("Authentication request: "MACSTR" %d %d %d %d\n",
			MAC2STR(cmd->bssid), cmd->channel, cmd->auth_type, cmd->auth_data_len,
			(u32) req->ie_len);
#if 0
	do {
		bss1 = cfg80211_get_bss(adapter->wiphy, bss->channel, bss->bssid,
				NULL, 0, IEEE80211_BSS_TYPE_ESS, IEEE80211_PRIVACY_ANY);

		if (bss1) {
			break;
		} else {
			esp_info("No BSS in the list.. scanning..\n");
			internal_scan_request(priv, cmd->ssid, cmd->channel, true);
		}

		retry--;
	} while (retry);
#endif
	spin_lock_bh(&priv->bss_lock);
	if (priv->auth_cmd_pending || priv->auth_awaiting_mlme) {
		spin_unlock_bh(&priv->bss_lock);
		recycle_cmd_node(adapter, cmd_node);
		return -EBUSY;
	}
	memcpy(priv->auth_bssid, bss->bssid, MAC_ADDR_LEN);
	priv->auth_cmd_seq = cmd_node->cmd_seq;
	spin_unlock_bh(&priv->bss_lock);
	esp_mlme_begin(priv, ESP_MLME_AUTH);
	queue_cmd_node(adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	ret = wait_and_decode_cmd_resp(priv, cmd_node);
	esp_mlme_end(priv, ESP_MLME_AUTH, ret == 0);
	if (ret) {
		esp_wifi_clear_auth_pending(priv);
		return ret;
	}

	/* Keep an independent BSS reference after .auth returns. */
	esp_wifi_ref_bss(priv, req->bss);

	return 0;
}

int cmd_mgmt_request(struct esp_wifi_device *priv,
		     struct cfg80211_mgmt_tx_params *req, u64 *cookie)
{
	struct command_node *cmd_node;
	struct cmd_mgmt_tx *cmd;
	size_t total_len;
	u64 tx_id;
	int ret;

	if (!priv || !priv->adapter || !req || !cookie || !req->buf || !req->len)
		return -EINVAL;
	if (req->len > U32_MAX ||
	    esp_size_add_overflow(sizeof(struct cmd_mgmt_tx), req->len, &total_len) ||
	    total_len > U16_MAX)
		return -EMSGSIZE;

	spin_lock_bh(&priv->bss_lock);
	if (priv->pending_mgmt_active) {
		if (time_after(jiffies, priv->pending_mgmt_sent_at + msecs_to_jiffies(7000))) {
			esp_warn("MGMT_TX stale active transaction expired (cookie=0x%llx)\n",
				 priv->pending_mgmt_cookie);
			priv->pending_mgmt_active = false;
			priv->pending_mgmt_cookie = 0;
			priv->pending_mgmt_id = 0;
		} else {
			spin_unlock_bh(&priv->bss_lock);
			return -EBUSY;
		}
	}
	priv->mgmt_tx_id++;
	if (priv->mgmt_tx_id == 0)
		priv->mgmt_tx_id = 1;
	tx_id = priv->mgmt_tx_id;
	priv->pending_mgmt_id = tx_id;
	priv->pending_mgmt_active = true;
	priv->pending_mgmt_sent_at = jiffies;
	spin_unlock_bh(&priv->bss_lock);

	cmd_node = prepare_command_request(priv->adapter, CMD_MGMT_TX, total_len);
	if (IS_ERR_OR_NULL(cmd_node)) {
		spin_lock_bh(&priv->bss_lock);
		priv->pending_mgmt_active = false;
		spin_unlock_bh(&priv->bss_lock);
		return cmd_prepare_err(cmd_node);
	}

	spin_lock_bh(&priv->bss_lock);
	priv->pending_mgmt_cookie = (u64)cmd_node->cmd_seq;
	spin_unlock_bh(&priv->bss_lock);

	cmd_node->cookie = (u64)cmd_node->cmd_seq;
	*cookie = cmd_node->cookie;
	cmd_node->mgmt_dont_wait_for_ack = req->dont_wait_for_ack;
	cmd_node->mgmt_frame = kmemdup(req->buf, req->len, GFP_KERNEL);
	if (!cmd_node->mgmt_frame) {
		spin_lock_bh(&priv->bss_lock);
		priv->pending_mgmt_active = false;
		spin_unlock_bh(&priv->bss_lock);
		recycle_cmd_node(priv->adapter, cmd_node);
		return -ENOMEM;
	}
	cmd_node->mgmt_frame_len = req->len;

	cmd = (struct cmd_mgmt_tx *)(cmd_node->cmd_skb->data +
				      sizeof(struct esp_payload_header));
	cmd->header.reserved1 = HOSTED_MGMT_TX_ASYNC_STATUS_V1;
	cmd->channel = req->chan ? req->chan->hw_value : 0;
	cmd->len = esp_wire_cpu_to_le32((u32)req->len);
	cmd->offchan = req->offchan;
	cmd->wait = esp_wire_cpu_to_le32(req->wait);
	cmd->no_cck = req->no_cck;
	cmd->dont_wait_for_ack = req->dont_wait_for_ack;
	cmd->mgmt_tx_id = esp_wire_cpu_to_le64(tx_id);
	memcpy(cmd->buf, req->buf, req->len);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);
	ret = wait_and_decode_cmd_resp(priv, cmd_node);
	if (ret) {
		spin_lock_bh(&priv->bss_lock);
		if (priv->pending_mgmt_id == tx_id)
			priv->pending_mgmt_active = false;
		spin_unlock_bh(&priv->bss_lock);
	}
	return ret;
}



int cmd_set_default_key(struct esp_wifi_device *priv, u8 key_index)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;
	struct cmd_key_operation *cmd;
	struct wifi_sec_key *key = NULL;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

#if 0
	if (key_index > ESP_MAX_KEY_INDEX) {
		esp_err("invalid key index[%u] > max[%u]\n",
				key_index, ESP_MAX_KEY_INDEX);
		return -EINVAL;
	}
#endif
	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags)) {
		esp_err(":%u cleanup in progress, return", __LINE__);
		return 0;
	}

	cmd_len = sizeof(struct cmd_key_operation);

	/* get new cmd node */
	cmd_node = prepare_command_request(priv->adapter, CMD_SET_DEFAULT_KEY, cmd_len);
	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	/* cmd specific update */
	cmd = (struct cmd_key_operation *) (cmd_node->cmd_skb->data +
			sizeof(struct esp_payload_header));
	key = &cmd->key;

	key->index = esp_wire_cpu_to_le32(key_index);
	key->set_cur = 1;

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_del_key(struct esp_wifi_device *priv, u8 key_index, bool pairwise,
		const u8 *mac_addr)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;
	struct cmd_key_operation *cmd;
	struct wifi_sec_key *key = NULL;
	const u8 *mac = NULL;
	const u8 bc_mac[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags))
		return 0;

#if 0
	if (key_index > ESP_MAX_KEY_INDEX) {
		esp_err("invalid key index[%u] > max[%u]\n",
				key_index, ESP_MAX_KEY_INDEX);
		return -EINVAL;
	}
#endif

	mac = pairwise ? mac_addr : bc_mac;

	cmd_len = sizeof(struct cmd_key_operation);

	/* get new cmd node */
	cmd_node = prepare_command_request(priv->adapter, CMD_DEL_KEY, cmd_len);
	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	/* cmd specific update */
	cmd = (struct cmd_key_operation *) (cmd_node->cmd_skb->data +
			sizeof(struct esp_payload_header));
	key = &cmd->key;

	if (mac && !is_multicast_ether_addr(mac))
		memcpy((char *)&key->mac_addr, (void *)mac, MAC_ADDR_LEN);

	key->index = esp_wire_cpu_to_le32(key_index);
	key->del = 1;

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_add_key(struct esp_wifi_device *priv, u8 key_index, bool pairwise,
		const u8 *mac_addr, struct key_params *params)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;
	struct cmd_key_operation *cmd;
	struct wifi_sec_key *key = NULL;
	const u8 bc_mac[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	const u8 *mac = NULL;

	if (!priv || !priv->adapter || !params ||
	    !params->key || !params->key_len) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	esp_verbose("key_idx: %u pairwise: %u params->key_len: %u\n"
                     "params->seq_len:%u params->mode: 0x%x\n"
                     "params->cipher: 0x%x\n",
                     key_index, pairwise, params->key_len, params->seq_len,
#if (LINUX_VERSION_CODE > KERNEL_VERSION(5, 1, 21))
		     params->mode,
#else
		     0,
#endif
		     params->cipher);

#if 0
	if (key_index > ESP_MAX_KEY_INDEX) {
		esp_err("invalid key index[%u] > max[%u]\n",
				key_index, ESP_MAX_KEY_INDEX);
		return -EINVAL;
	}
#endif

	if (params->key_len > sizeof(key->data)) {
		esp_err("Too long key length (%u)\n", params->key_len);
		return -EINVAL;
	}

	if (params->seq_len > sizeof(key->seq)) {
		esp_err("Too long key seq length (%u)\n", params->seq_len);
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags)) {
		esp_err("%u cleanup in progress, return failure", __LINE__);
		return -EFAULT;
	}

	mac = pairwise ? mac_addr : bc_mac;
	if (mac) {
		esp_hex_dump_verbose("mac: ", mac, MAC_ADDR_LEN);
	}

	cmd_len = sizeof(struct cmd_key_operation);

	cmd_node = prepare_command_request(priv->adapter, CMD_ADD_KEY, cmd_len);
	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd = (struct cmd_key_operation *) (cmd_node->cmd_skb->data +
			sizeof(struct esp_payload_header));
	key = &cmd->key;

	if (mac && !is_multicast_ether_addr(mac))
		memcpy((char *)&key->mac_addr, (void *)mac, MAC_ADDR_LEN);

	key->index = esp_wire_cpu_to_le32(key_index);

	key->len = esp_wire_cpu_to_le32(params->key_len);
	if (params->key && params->key_len)
		memcpy(key->data, params->key, params->key_len);

	key->seq_len = esp_wire_cpu_to_le32(params->seq_len);
	if (params->seq && params->seq_len)
		memcpy(key->seq, params->seq, params->seq_len);

	key->algo = esp_wire_cpu_to_le32(wpa_cipher_to_alg(params->cipher));
#if 0
	if (key->algo == WIFI_WPA_ALG_NONE) {
		esp_info("CIPHER NONE does not use pairwise keys\n");
		return 0;
	}
#endif

       /* Supplicant swaps tx/rx Mic keys whereas esp needs it normal format */
       if (key->algo == WIFI_WPA_ALG_TKIP) {
               u8 buf[8];
               memcpy(buf, &key->data[16], 8);
               memcpy(&key->data[16], &key->data[24], 8);
               memcpy(&key->data[24], buf, 8);
               memset(buf, 0, 8);
       }

	esp_verbose("algo: %u idx: %u seq_len: %u len:%u\n",
			key->algo, key->index, key->seq_len, key->len);
	esp_hex_dump_verbose("mac", key->mac_addr, 6);
	esp_hex_dump_verbose("seq", key->seq, key->seq_len);
	esp_hex_dump_verbose("key_data", key->data, key->len);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_update_fw_time(struct esp_wifi_device *priv)
{
	struct command_node *cmd_node;
	struct cmd_set_time *val;
	struct timespec64 ts;

	if (!priv || !priv->adapter)
		return -EINVAL;
	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags))
		return 0;

	ktime_get_real_ts64(&ts);
	cmd_node = prepare_command_request(priv->adapter, CMD_SET_TIME,
					   sizeof(struct cmd_set_time));
	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	val = (struct cmd_set_time *)(cmd_node->cmd_skb->data +
				       sizeof(struct esp_payload_header));
	val->sec = esp_wire_cpu_to_le64((u64)ts.tv_sec);
	val->usec = esp_wire_cpu_to_le64((u64)(ts.tv_nsec / 1000));

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);
	return wait_and_decode_cmd_resp(priv, cmd_node);
}

int cmd_init_interface(struct esp_wifi_device *priv)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	cmd_len = sizeof(struct command_header);

	cmd_node = prepare_command_request(priv->adapter, CMD_INIT_INTERFACE, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_deinit_interface(struct esp_wifi_device *priv)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;

	if (!priv || !priv->adapter)
		return -EINVAL;
	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags) &&
	    !test_bit(ESP_ALLOW_DEINIT, &priv->adapter->state_flags))
		return 0;

	cmd_len = sizeof(struct command_header);

	cmd_node = prepare_command_request(priv->adapter, CMD_DEINIT_INTERFACE, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int internal_scan_request(struct esp_wifi_device *priv, char *ssid,
		uint8_t channel, uint8_t is_blocking)
{
	int ret = 0;
	u16 cmd_len;
	struct command_node *cmd_node = NULL;
	struct scan_request *scan_req;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (priv->scan_in_progress) {
		esp_err("Scan in progress.. return\n");
		return -EBUSY;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags)) {
		esp_err("%u cleanup in progress, return", __LINE__);
		return -EBUSY;
	}

	cmd_len = sizeof(struct scan_request);

	cmd_node = prepare_command_request(priv->adapter, CMD_SCAN_REQUEST, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	scan_req = (struct scan_request *) (cmd_node->cmd_skb->data +
			sizeof(struct esp_payload_header));

	if (ssid) {
		memcpy(scan_req->ssid, ssid, MAX_SSID_LEN);
	}

	scan_req->channel = channel;

	priv->scan_in_progress = true;

	if (is_blocking)
		priv->waiting_for_scan_done = true;

	/* Enqueue command */
	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	ret = wait_and_decode_cmd_resp(priv, cmd_node);

	if (!ret && is_blocking) {
		/* Wait for scan done */
		ret = wait_event_interruptible_timeout(priv->wait_for_scan_completion,
				priv->waiting_for_scan_done != true, SCAN_COMPLETION_TIMEOUT);
		if (ret == 0) {
			esp_err("SCAN_DONE_TIMEOUT after command response\n");
			priv->waiting_for_scan_done = false;
			ESP_MARK_SCAN_DONE(priv, true);
			ret = -ETIMEDOUT;
		} else if (ret < 0) {
			esp_err("SCAN_DONE_WAIT_INTERRUPTED ret=%d\n", ret);
			priv->waiting_for_scan_done = false;
			ESP_MARK_SCAN_DONE(priv, true);
		} else {
			ret = 0;
		}
	}

	return ret;
}

int cmd_scan_request(struct esp_wifi_device *priv, struct cfg80211_scan_request *request)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;
	struct scan_request *scan_req;
	int ret;
	int scan_eligible_channels = 0;
	enum nl80211_band band;
	int i;

	if (!priv || !priv->adapter || !request) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (request->n_ssids > 1) {
		esp_err("Multiple SSIDs in scan request not supported: %u\n",
			request->n_ssids);
		return -EOPNOTSUPP;
	}

	if (request->ie && request->ie_len) {
		esp_err("Scan IEs not supported\n");
		return -EOPNOTSUPP;
	}

	if (request->n_channels > 1) {
		if (priv->adapter->wiphy) {
			for (band = 0; band < NUM_NL80211_BANDS; band++) {
				struct ieee80211_supported_band *sband;

				sband = priv->adapter->wiphy->bands[band];
				if (!sband)
					continue;
				for (i = 0; i < sband->n_channels; i++) {
					if (!(sband->channels[i].flags & IEEE80211_CHAN_DISABLED))
						scan_eligible_channels++;
				}
			}
		}
		if (scan_eligible_channels > 0 && request->n_channels < scan_eligible_channels) {
			esp_err("Channel subset scan not supported (%u of %d scan-eligible channels)\n",
				request->n_channels, scan_eligible_channels);
			return -EOPNOTSUPP;
		}
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags)) {
		esp_err("%u cleanup in progress, return", __LINE__);
		return -EBUSY;
	}

	if (priv->scan_in_progress || priv->request) {
		esp_err("Scan in progress.. return\n");
		return -EBUSY;
	}

	cmd_len = sizeof(struct scan_request);

	cmd_node = prepare_command_request(priv->adapter, CMD_SCAN_REQUEST, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	scan_req = (struct scan_request *) (cmd_node->cmd_skb->data +
			sizeof(struct esp_payload_header));

	/* TODO: Handle case of multiple SSIDs or channels */
	if (request->ssids && request->ssids[0].ssid_len) {
		memcpy(scan_req->ssid, request->ssids[0].ssid, MAX_SSID_LEN);
	}

#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 8, 0)
	scan_req->duration = request->duration;
#endif
#if LINUX_VERSION_CODE > KERNEL_VERSION(4, 7, 0)
	memcpy(scan_req->bssid, request->bssid, MAC_ADDR_LEN);
#endif

	if (request->n_channels == 1 && request->channels && request->channels[0])
		scan_req->channel = request->channels[0]->hw_value;
	else
		scan_req->channel = 0;

	priv->scan_in_progress = true;
	priv->request = request;

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	ret = wait_and_decode_cmd_resp(priv, cmd_node);
	if (ret) {
		priv->scan_in_progress = false;
		priv->request = NULL;
		return ret;
	}

	schedule_delayed_work(&priv->scan_timeout_work, msecs_to_jiffies(15000));
	return 0;
}

int esp_ota_finish_or_recover(struct esp_wifi_device *priv, int ret)
{
	struct esp_adapter *adapter;

	if (!ret || !priv || !priv->adapter)
		return ret;
	adapter = priv->adapter;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return ret;

	/* There is no OTA abort/status transaction in the established ABI. Once
	 * an OTA stage fails, the firmware may still own a begun/partially-written
	 * handle or the side effect may have committed despite a lost response.
	 * The only safe reconciliation is a fresh firmware incarnation. */
	if (!test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags)) {
		esp_err("OTA stage failed ret=%d: forcing firmware reincarnation\n", ret);
		esp_schedule_fw_reset_recovery(adapter);
		esp_request_firmware_restart(adapter);
	}
	return ret;
}

int cmd_process_ota_start(struct esp_wifi_device *priv)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;
	int ret;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	cmd_len = sizeof(struct command_header);

	cmd_node = prepare_command_request(priv->adapter, CMD_START_OTA_UPDATE, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	ret = wait_and_decode_cmd_resp(priv, cmd_node);

	return esp_ota_finish_or_recover(priv, ret);
}

int cmd_process_ota_write(struct esp_wifi_device *priv, char *ota_chunk, ssize_t nread)
{
	size_t cmd_len;
	struct command_node *cmd_node = NULL;
	struct cmd_ota_update_request * cmd_ota_req = NULL;
	int ret;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}
	if (nread < 0 || nread > OTA_CHUNK_SIZE) {
		esp_err("OTA write length invalid: %zd max=%u\n", nread, OTA_CHUNK_SIZE);
		return esp_ota_finish_or_recover(priv, -EINVAL);
	}

	if (esp_size_add_overflow(sizeof(struct cmd_ota_update_request),
				  (size_t)nread, &cmd_len))
		return esp_ota_finish_or_recover(priv, -EOVERFLOW);

	cmd_node = prepare_command_request(priv->adapter, CMD_START_OTA_WRITE, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return esp_ota_finish_or_recover(priv, cmd_prepare_err(cmd_node));

	cmd_ota_req = (struct cmd_ota_update_request *) (cmd_node->cmd_skb->data +
				sizeof(struct esp_payload_header));

	cmd_ota_req->ota_binary_len = esp_wire_cpu_to_le16((u16)nread);
	memcpy(cmd_ota_req->ota_binary, ota_chunk, nread);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	ret = wait_and_decode_cmd_resp(priv, cmd_node);

	return esp_ota_finish_or_recover(priv, ret);
}

int cmd_process_ota_end(struct esp_wifi_device *priv)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;
	int ret;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	cmd_len = sizeof(struct command_header);

	cmd_node = prepare_command_request(priv->adapter, CMD_START_OTA_END, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return esp_ota_finish_or_recover(priv, cmd_prepare_err(cmd_node));

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	ret = wait_and_decode_cmd_resp(priv, cmd_node);

	return esp_ota_finish_or_recover(priv, ret);
}

int cmd_init_raw_tp_task_timer(struct esp_wifi_device *priv)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;
	struct cmd_raw_tp *cmd = NULL;
	u32 run_id;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	cmd_len = sizeof(struct cmd_raw_tp);
	run_id = esp_raw_tp_alloc_run_id();

	if (raw_tp_mode == ESP_TEST_RAW_TP_ESP_TO_HOST) {
		cmd_node = prepare_command_request(priv->adapter, CMD_RAW_TP_ESP_TO_HOST, cmd_len);
	} else if (raw_tp_mode == ESP_TEST_RAW_TP_HOST_TO_ESP) {
		cmd_node = prepare_command_request(priv->adapter, CMD_RAW_TP_HOST_TO_ESP, cmd_len);
	}

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd = (struct cmd_raw_tp *)(cmd_node->cmd_skb->data +
				    sizeof(struct esp_payload_header));
	cmd->run_id = esp_wire_cpu_to_le32(run_id);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_get_rssi(struct esp_wifi_device *priv)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (priv->if_type != ESP_STA_IF) {
		esp_err("Invalid interface\n");
		return -EINVAL;
	}

	cmd_len = sizeof(struct command_header) + sizeof(int32_t);

	cmd_node = prepare_command_request(priv->adapter, CMD_STA_RSSI, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_get_mac(struct esp_wifi_device *priv)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	cmd_len = sizeof(struct command_header);

	cmd_node = prepare_command_request(priv->adapter, CMD_GET_MAC, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_set_mac(struct esp_wifi_device *priv, uint8_t *mac_addr)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;
	struct cmd_config_mac_address *cmd;;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	cmd_len = sizeof(struct cmd_config_mac_address);

	cmd_node = prepare_command_request(priv->adapter, CMD_SET_MAC, cmd_len);
	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd = (struct cmd_config_mac_address *) (cmd_node->cmd_skb->data +
				sizeof(struct esp_payload_header));

	memcpy(cmd->mac_addr, mac_addr, MAC_ADDR_LEN);
	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));
	return 0;
}

int cmd_set_mode(struct esp_wifi_device *priv, uint8_t mode)
{
	struct command_node *cmd_node = NULL;
	struct cmd_config_mode *cmd_mode;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags))
		return 0;

	cmd_node = prepare_command_request(priv->adapter, CMD_SET_MODE,
			sizeof(struct cmd_config_mode));

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd_mode = (struct cmd_config_mode *)
		(cmd_node->cmd_skb->data + sizeof(struct esp_payload_header));

	cmd_mode->mode = esp_wire_cpu_to_le16(mode);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));
	return 0;
}

int cmd_stop_ap(struct esp_wifi_device *priv)
{
	/* WIFI_MODE_STA is 1 in the existing host/firmware wire ABI. No new
	 * command code is needed for the single-interface AP teardown. */
	if (!priv || !priv->adapter || priv->if_type != ESP_AP_IF)
		return -EINVAL;
	return cmd_set_mode(priv, 1);
}

int cmd_set_ie(struct esp_wifi_device *priv, enum ESP_IE_TYPE type, const uint8_t *ie, size_t ie_len)
{
	struct command_node *cmd_node = NULL;
	struct cmd_config_ie *cmd_ie;
	size_t cmd_len;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags))
		return 0;

	if (ie_len > U16_MAX || (ie_len && !ie)) {
		esp_err("SET_IE too large or missing: type=%u len=%zu\n", type, ie_len);
		return -EINVAL;
	}

	if (esp_size_add_overflow(sizeof(struct cmd_config_ie), ie_len, &cmd_len))
		return -EOVERFLOW;
	cmd_node = prepare_command_request(priv->adapter, CMD_SET_IE, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd_ie = (struct cmd_config_ie *)
		(cmd_node->cmd_skb->data + sizeof(struct esp_payload_header));

	cmd_ie->ie_type = type;
	cmd_ie->ie_len = esp_wire_cpu_to_le16((u16)ie_len);
	if (ie_len && ie)
		memcpy(cmd_ie->ie, ie, ie_len);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));
	return 0;
}


int cmd_set_ap_config(struct esp_wifi_device *priv, struct esp_ap_config *ap_config)
{
	struct command_node *cmd_node = NULL;
	struct cmd_ap_config *cmd_config;
	size_t cmd_len;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (priv->if_type != ESP_AP_IF) {
		esp_err("Invalid interface\n");
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags))
		return 0;

	cmd_len = sizeof(struct cmd_ap_config);
	cmd_node = prepare_command_request(priv->adapter, CMD_AP_CONFIG, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd_config = (struct cmd_ap_config *)
		(cmd_node->cmd_skb->data + sizeof(struct esp_payload_header));

	memcpy(&cmd_config->ap_config, ap_config, sizeof(struct esp_ap_config));
	cmd_config->ap_config.beacon_interval = esp_wire_cpu_to_le16(ap_config->beacon_interval);
	cmd_config->ap_config.inactivity_timeout = esp_wire_cpu_to_le16(ap_config->inactivity_timeout);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));
	return 0;
}

int esp_cfg_cleanup(struct esp_adapter *adapter)
{
	struct esp_wifi_device *priv = NULL;
	uint8_t iface_idx = 0;

	for (iface_idx = 0; iface_idx < ESP_MAX_INTERFACE; iface_idx++) {
		priv = adapter->priv[iface_idx];
		if (!priv)
			continue;

		if (priv->wdev.iftype == NL80211_IFTYPE_STATION)
			esp_mark_scan_done_and_disconnect(priv, false);

		esp_port_close(priv);
	}

	return 0;
}

int esp_commands_teardown(struct esp_adapter *adapter)
{
	if (!adapter)
		return -EINVAL;

	if (!test_bit(ESP_CMD_INIT_DONE, &adapter->state_flags))
		return 0;

	spin_lock_bh(&adapter->cmd_lock);
	clear_bit(ESP_CMD_INIT_DONE, &adapter->state_flags);
	spin_unlock_bh(&adapter->cmd_lock);

	esp_cmd_abort_waiters(adapter);

	destroy_cmd_wq(adapter);
	if (!wait_event_timeout(adapter->wait_for_cmd_resp,
			atomic_read(&adapter->cmd_nodes_in_use) == 0,
			COMMAND_RESPONSE_TIMEOUT)) {
		esp_err("Command teardown timed out with %d active command nodes; keeping cmd_pool allocated\n",
				atomic_read(&adapter->cmd_nodes_in_use));
		return -EBUSY;
	}
	free_esp_cmd_pool(adapter);

	return 0;
}

void esp_cmd_abort_waiters(struct esp_adapter *adapter)
{
	struct command_node *cmd_node;
	struct command_node *tmp;

	if (!adapter)
		return;

	if (!test_bit(ESP_CMD_INIT_DONE, &adapter->state_flags) &&
	    !adapter->cmd_wq)
		return;

	spin_lock_bh(&adapter->cmd_lock);
	if (adapter->cur_cmd && !adapter->cur_cmd->completed) {
		adapter->cur_cmd->result = -ESHUTDOWN;
		adapter->cur_cmd->completed = true;
	}
	spin_lock_bh(&adapter->cmd_pending_queue_lock);
	list_for_each_entry_safe(cmd_node, tmp,
			&adapter->cmd_pending_queue, list) {
		list_del_init(&cmd_node->list);
		cmd_node->in_pending_queue = false;
		cmd_node->result = -ESHUTDOWN;
		cmd_node->completed = true;
	}
	spin_unlock_bh(&adapter->cmd_pending_queue_lock);
	spin_unlock_bh(&adapter->cmd_lock);
	wake_up_all(&adapter->wait_for_cmd_resp);
}

int esp_commands_setup(struct esp_adapter *adapter)
{
	int ret;

	if (!adapter) {
		esp_err("no adapter\n");
		return -EINVAL;
	}

	if (adapter->cmd_pool) {
		if (atomic_read(&adapter->cmd_nodes_in_use)) {
			esp_err("Cannot setup command queue with %d active command nodes\n",
					atomic_read(&adapter->cmd_nodes_in_use));
			return -EBUSY;
		}

		free_esp_cmd_pool(adapter);
	}

	init_waitqueue_head(&adapter->wait_for_cmd_resp);
	atomic_set(&adapter->cmd_nodes_in_use, 0);

	spin_lock_init(&adapter->cmd_lock);

	INIT_LIST_HEAD(&adapter->cmd_pending_queue);
	INIT_LIST_HEAD(&adapter->cmd_free_queue);

	spin_lock_init(&adapter->cmd_pending_queue_lock);
	spin_lock_init(&adapter->cmd_free_queue_lock);
	adapter->cur_cmd = NULL;
	adapter->cmd_resp = 0;
	adapter->cmd_resp_seq = 0;
	adapter->next_cmd_seq = 0;

	ret = create_cmd_wq(adapter);
	if (ret)
		return ret;

	ret = alloc_esp_cmd_pool(adapter);
	if (ret) {
		destroy_cmd_wq(adapter);
		return ret;
	}

	set_bit(ESP_CMD_INIT_DONE, &adapter->state_flags);
	return 0;
}

int cmd_set_wow_config(struct esp_wifi_device *priv, struct cfg80211_wowlan *wowlan)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;
	struct cmd_wow_config *config;;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	cmd_len = sizeof(struct cmd_wow_config);

	cmd_node = prepare_command_request(priv->adapter, CMD_SET_WOW_CONFIG, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	config = (struct cmd_wow_config *) (cmd_node->cmd_skb->data +
				sizeof(struct esp_payload_header));

	config->any = wowlan->any;
	config->disconnect = wowlan->disconnect;
	config->magic_pkt = wowlan->magic_pkt;
	config->four_way_handshake = wowlan->four_way_handshake;
	config->eap_identity_req = wowlan->eap_identity_req;

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_set_tx_power(struct esp_wifi_device *priv, int power)
{
	struct command_node *cmd_node;
	struct cmd_set_get_val *val;

	if (!priv || !priv->adapter)
		return -EINVAL;
	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags))
		return 0;

	cmd_node = prepare_command_request(priv->adapter, CMD_SET_TXPOWER,
					   sizeof(struct cmd_set_get_val));
	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	val = (struct cmd_set_get_val *)(cmd_node->cmd_skb->data +
					 sizeof(struct esp_payload_header));
	val->value = esp_wire_cpu_to_le32((u32)power);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);
	return wait_and_decode_cmd_resp(priv, cmd_node);
}

int cmd_add_station(struct esp_wifi_device *priv, const uint8_t *mac,
		    struct station_parameters *sta, bool is_changed)
{
	struct command_node *cmd_node = NULL;
	struct cmd_ap_add_sta_config *cmd_config;
	size_t cmd_len;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0))
	struct link_station_parameters *rate_params = &sta->link_sta_params;
#else
	struct station_parameters *rate_params = sta;
#endif

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	if (priv->if_type != ESP_AP_IF) {
		esp_err("Invalid interface\n");
		return -EINVAL;
	}

	if (test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags))
		return 0;

	cmd_len = sizeof(struct cmd_ap_add_sta_config);
	cmd_node = prepare_command_request(priv->adapter, CMD_AP_STATION, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd_config = (struct cmd_ap_add_sta_config *)
		(cmd_node->cmd_skb->data + sizeof(struct esp_payload_header));

	memcpy(cmd_config->sta_param.mac, mac, 6);
	if (is_changed)
		cmd_config->sta_param.cmd = esp_wire_cpu_to_le16(CHANGE_STA);
	else
		cmd_config->sta_param.cmd = esp_wire_cpu_to_le16(ADD_STA);
	cmd_config->sta_param.sta_flags_mask = esp_wire_cpu_to_le32(sta->sta_flags_mask);
	cmd_config->sta_param.sta_flags_set = esp_wire_cpu_to_le32(sta->sta_flags_set);
	cmd_config->sta_param.sta_modify_mask = esp_wire_cpu_to_le32(sta->sta_modify_mask);
	cmd_config->sta_param.listen_interval = esp_wire_cpu_to_le32(sta->listen_interval);
	cmd_config->sta_param.aid = esp_wire_cpu_to_le16(sta->aid);

	if (sta->ext_capab_len && sta->ext_capab) {
		if (sta->ext_capab_len > 4)
			sta->ext_capab_len = 4;
		memcpy(cmd_config->sta_param.ext_capab, sta->ext_capab, sta->ext_capab_len);
	}

	if (rate_params->supported_rates_len && rate_params->supported_rates) {
		size_t rates_len = min_t(size_t, rate_params->supported_rates_len, 10);
		cmd_config->sta_param.supported_rates[0] = WLAN_EID_SUPP_RATES;
		cmd_config->sta_param.supported_rates[1] = rates_len;
		memcpy(&cmd_config->sta_param.supported_rates[2], rate_params->supported_rates, rates_len);
	}

	if (rate_params->ht_capa) {
		cmd_config->sta_param.ht_caps[0] = WLAN_EID_HT_CAPABILITY;
		cmd_config->sta_param.ht_caps[1] = 26;
		memcpy(&cmd_config->sta_param.ht_caps[2], rate_params->ht_capa, 26);
	}
	if (rate_params->vht_capa) {
		cmd_config->sta_param.vht_caps[0] = WLAN_EID_VHT_CAPABILITY;
		cmd_config->sta_param.vht_caps[1] = 12;
		memcpy(&cmd_config->sta_param.vht_caps[2], rate_params->vht_capa, 12);
	}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0))
	if (rate_params->he_capa && rate_params->he_capa_len) {
		size_t he_len = min_t(size_t, rate_params->he_capa_len, 24);
		cmd_config->sta_param.he_caps[0] = WLAN_EID_EXTENSION;
		cmd_config->sta_param.he_caps[1] = he_len + 1;
		cmd_config->sta_param.he_caps[2] = WLAN_EID_EXT_HE_CAPABILITY;
		memcpy(&cmd_config->sta_param.he_caps[3], rate_params->he_capa, he_len);
	}
#endif

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_get_tx_power(struct esp_wifi_device *priv)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	cmd_len = sizeof(struct command_header) + sizeof(int32_t);

	cmd_node = prepare_command_request(priv->adapter, CMD_GET_TXPOWER, cmd_len);
	if (IS_ERR(cmd_node) && PTR_ERR(cmd_node) == -EBUSY)
		return 0;
	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_get_reg_domain(struct esp_wifi_device *priv)
{
	u16 cmd_len;
	struct command_node *cmd_node = NULL;

	if (!priv || !priv->adapter) {
		esp_err("Invalid argument\n");
		return -EINVAL;
	}

	cmd_len = sizeof(struct cmd_reg_domain);

	cmd_node = prepare_command_request(priv->adapter, CMD_GET_REG_DOMAIN, cmd_len);

	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);

	RET_ON_FAIL(wait_and_decode_cmd_resp(priv, cmd_node));

	return 0;
}

int cmd_set_reg_domain(struct esp_wifi_device *priv, const char *country_code)
{
	struct command_node *cmd_node;
	struct cmd_reg_domain *cmd;
	int ret;

	if (!priv || !priv->adapter || !country_code)
		return -EINVAL;
	if (!country_code[0] || !country_code[1])
		return -EINVAL;

	cmd_node = prepare_command_request(priv->adapter, CMD_SET_REG_DOMAIN,
					   sizeof(struct cmd_reg_domain));
	if (IS_ERR_OR_NULL(cmd_node))
		return cmd_prepare_err(cmd_node);

	cmd = (struct cmd_reg_domain *)(cmd_node->cmd_skb->data +
					 sizeof(struct esp_payload_header));
	cmd->country_code[0] = country_code[0];
	cmd->country_code[1] = country_code[1];
	cmd->country_code[2] = '\0';
	cmd->country_code[3] = '\0';

	queue_cmd_node(priv->adapter, cmd_node, ESP_CMD_DFLT_PRIO);
	ret = wait_and_decode_cmd_resp(priv, cmd_node);
	if (!ret) {
		/* Commit the notifier cache only after firmware acknowledged apply. */
		priv->country_code[0] = country_code[0];
		priv->country_code[1] = country_code[1];
		priv->country_code[2] = '\0';
	}
	return ret;
}
