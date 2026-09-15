// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 */
#include "utils.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/gpio.h>
#include <linux/igmp.h>
#include <linux/jiffies.h>

#include "esp.h"
#include "esp_if.h"
#include "esp_bt_api.h"
#include "esp_api.h"
#include "esp_cmd.h"
#include "esp_kernel_port.h"
#include "esp_fw_version.h"

#include "esp_cfg80211.h"
#include "esp_stats.h"

#define HOST_GPIO_PIN_INVALID -1
#define CONFIG_ALLOW_MULTICAST_WAKEUP 1

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

#define RELEASE_VERSION PROJECT_NAME "-" STRINGIFY(PROJECT_VERSION_MAJOR_1) "." STRINGIFY(PROJECT_VERSION_MAJOR_2) "." STRINGIFY(PROJECT_VERSION_MINOR) "." STRINGIFY(PROJECT_REVISION_PATCH_1) "." STRINGIFY(PROJECT_REVISION_PATCH_2)

static char *ota_file = NULL;
static int resetpin = HOST_GPIO_PIN_INVALID;
static u32 clockspeed = 0;
u32 raw_tp_mode = 0;
int log_level = ESP_INFO;
char version_str[ESP_VERSION_BUFFER_SIZE];


module_param(resetpin, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(resetpin, "Host's GPIO pin number which is connected to ESP32's EN to reset ESP32 device");

module_param(clockspeed, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(clockspeed, "Hosts clock speed in MHz");

module_param(raw_tp_mode, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(raw_tp_mode, "Mode chosen to test raw throughput");

module_param(ota_file, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(ota_file, "Ota file to update ESP firmware");

static void deinit_adapter(void);
static void esp_reset(void);
static int esp_publish_network_ifaces(struct esp_adapter *adapter);


static struct multicast_list mcast_list = {0};
static struct esp_adapter adapter;
/*struct esp_device esp_dev;*/

struct esp_adapter *esp_get_adapter(void)
{
	return &adapter;
}

bool esp_host_reset_available(void)
{
	return resetpin != HOST_GPIO_PIN_INVALID && gpio_is_valid(resetpin);
}

void esp_process_new_packet_intr(struct esp_adapter *adapter)
{
	if (!adapter || !adapter->if_rx_workqueue)
		return;
	if (test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
	    (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags)))
		return;

	queue_work(adapter->if_rx_workqueue, &adapter->if_rx_work);
}

static void esp_recovery_drop_tx_unlocked(struct esp_adapter *adapter)
{
	/* Only clamp READY → RX_READY. Never raise DISABLED: reconstruct fail
	 * quiesces to DISABLED, and a later schedule_recovery must not revive
	 * the ISR on a card that may already be gone. */
	if (atomic_read(&adapter->state) > ESP_CONTEXT_RX_READY)
		atomic_set(&adapter->state, ESP_CONTEXT_RX_READY);
}

void esp_schedule_recovery(struct esp_adapter *adapter,
		unsigned int delay_ms, bool restart_timer, bool fw_reset)
{
	unsigned long flags;

	if (!adapter)
		return;

	spin_lock_irqsave(&adapter->fw_recovery_lock, flags);
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags)) {
		spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);
		return;
	}

	if (fw_reset) {
		set_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags);
		if (adapter->if_ops && adapter->if_ops->note_fw_reset)
			adapter->if_ops->note_fw_reset(adapter);
	}

	set_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags);
	atomic_inc(&adapter->fw_recovery_gen);

	/* Reconstruction owns READY. Remember the request via PENDING/gen, but
	 * do not drop Host→ESP TX here: the worker returns without recovering
	 * until commit. */
	if (!test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags))
		esp_recovery_drop_tx_unlocked(adapter);
	spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);

	if (!delay_ms)
		delay_ms = ESP_FW_RECOVERY_QUIET_MS;

	/* Bus errors can fire continuously during a flash. Do not reset the
	 * timer on every error or recovery never runs. OTA/watchdog may. */
	if (restart_timer)
		mod_delayed_work(system_wq, &adapter->fw_recovery_work,
				 msecs_to_jiffies(delay_ms));
	else
		schedule_delayed_work(&adapter->fw_recovery_work,
				      msecs_to_jiffies(delay_ms));
}

void esp_schedule_transport_recovery(struct esp_adapter *adapter)
{
	esp_schedule_recovery(adapter, ESP_FW_RECOVERY_QUIET_MS, false, false);
}

void esp_schedule_fw_reset_recovery(struct esp_adapter *adapter)
{
	esp_schedule_recovery(adapter, ESP_FW_RECOVERY_QUIET_MS, false, true);
}

void esp_request_firmware_restart(struct esp_adapter *adapter)
{
	int ret = -ENODEV;

	if (!adapter)
		return;

	set_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);

	if (adapter->if_context)
		ret = generate_slave_intr(adapter->if_context,
					  BIT(ESP_CLOSE_DATA_PATH));

	/* SPI cannot deliver CLOSE_DATA_PATH. A valid reset GPIO is a required
	 * SPI capability, enforced during transport initialization. */
	if (adapter->if_type == ESP_IF_TYPE_SPI) {
		if (esp_host_reset_available()) {
			clear_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);
			esp_reset();
		} else {
			esp_err("SPI firmware restart requested without a usable reset GPIO\n");
		}
	} else if (ret == 0) {
		clear_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);
	} else {
		esp_info("CLOSE_DATA_PATH failed %d; scheduled for retry in transport recovery\n",
			 ret);
	}
}

static void esp_bootup_request_retry(struct esp_adapter *adapter)
{
	if (!adapter)
		return;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return;

	/* The boot TLV was already consumed. Repeated OPEN does not make
	 * firmware resend it; restart the slave so a new boot can be published. */
	esp_request_firmware_restart(adapter);

	set_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags);
	/* A failed reconstruct must not leave CLEANUP stuck, or recover_transport
	 * can never reopen the datapath while the module stays loaded. */
	clear_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
	esp_schedule_recovery(adapter, ESP_FW_RECOVERY_WATCHDOG_MS, true, true);
}

static void esp_fw_recovery_work(struct work_struct *work)
{
	struct esp_adapter *adapter = container_of(to_delayed_work(work),
			struct esp_adapter, fw_recovery_work);
	int ret;
	unsigned int delay;
	unsigned long flags;
	bool fw_reset;
	bool skip;
	u32 gen;

	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return;

	spin_lock_irqsave(&adapter->fw_recovery_lock, flags);
	skip = test_bit(ESP_INIT_DONE, &adapter->state_flags) &&
	       !test_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags) &&
	       !test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags);
	if (!skip && test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags)) {
		spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);
		esp_schedule_recovery(adapter, ESP_FW_RECOVERY_WATCHDOG_MS,
				      false, false);
		return;
	}
	if (!skip && test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags)) {
		spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);
		return;
	}
	if (skip) {
		spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);
		return;
	}
	fw_reset = test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags);
	gen = atomic_read(&adapter->fw_recovery_gen);
	spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);

	if (!adapter->if_ops || !adapter->if_ops->recover_transport) {
		esp_err("Transport recovery: backend has no recover_transport\n");
		return;
	}

	esp_info("Transport recovery: %s\n",
		 fw_reset ? "firmware reset expected" : "transport resync");
	ret = adapter->if_ops->recover_transport(adapter);
	if (ret < 0) {
		if (!adapter->fw_recovery_backoff_ms)
			adapter->fw_recovery_backoff_ms = ESP_FW_RECOVERY_QUIET_MS;
		delay = adapter->fw_recovery_backoff_ms;
		if (adapter->fw_recovery_backoff_ms < ESP_FW_RECOVERY_BACKOFF_MAX_MS) {
			adapter->fw_recovery_backoff_ms *= 2;
			if (adapter->fw_recovery_backoff_ms > ESP_FW_RECOVERY_BACKOFF_MAX_MS)
				adapter->fw_recovery_backoff_ms =
					ESP_FW_RECOVERY_BACKOFF_MAX_MS;
		}
		esp_err("Transport recovery: recover failed %d, retry in %u ms\n",
			ret, delay);
		/* Do not pass fw_reset here: note_fw_reset() would rebase again
		 * after OPEN may already have published a boot TLV. */
		esp_schedule_recovery(adapter, delay, false, false);
		return;
	}

	adapter->fw_recovery_backoff_ms = ESP_FW_RECOVERY_QUIET_MS;

	if (ret > 0) {
		spin_lock_irqsave(&adapter->fw_recovery_lock, flags);
		if (atomic_read(&adapter->fw_recovery_gen) != gen) {
			spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);
			esp_info("Transport recovery: newer reset request pending\n");
			return;
		}
		if (test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags)) {
			spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);
			esp_info("Transport recovery: firmware reset still expected, retrying\n");
			schedule_delayed_work(&adapter->fw_recovery_work,
					      msecs_to_jiffies(ESP_FW_RECOVERY_QUIET_MS));
			return;
		}
		clear_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags);
		spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);
		esp_info("Transport recovery: existing session restored\n");
		return;
	}

	/* ret == 0: OPEN issued, waiting for a new boot TLV. Retry OPEN (without
	 * rebasing) if the boot event never arrives. Do not go through
	 * schedule_in(): that would drop state to RX_READY again after a boot
	 * worker has already set READY for reconstruction. */
	if (!test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) &&
	    !test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) &&
	    (test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags) ||
	     !test_bit(ESP_INIT_DONE, &adapter->state_flags)))
		schedule_delayed_work(&adapter->fw_recovery_work,
				      msecs_to_jiffies(ESP_FW_RECOVERY_WATCHDOG_MS));
}

static int process_tx_packet(struct sk_buff *skb)
{
	struct esp_wifi_device *priv = NULL;
	struct esp_skb_cb *cb = NULL;
	struct esp_payload_header *payload_header = NULL;
	struct sk_buff *new_skb = NULL;
	int ret = 0;
	u8 pad_len = 0, realloc_skb = 0;
	u16 len = 0;
	u16 total_len = 0;
	static u8 c;
	u8 *pos = NULL;
	bool is_eapol;

	c++;
	/* Get the priv */
	cb = (struct esp_skb_cb *) skb->cb;
	priv = cb->priv;

	if (!priv) {
		dev_kfree_skb(skb);
		esp_info("No priv\n");
		return NETDEV_TX_OK;
	}

	if (netif_queue_stopped((const struct net_device *) priv->ndev)) {
		esp_info("Netif queue stopped\n");
		return NETDEV_TX_BUSY;
	}

	if (host_sleep) {
		return NETDEV_TX_BUSY;
	}
	len = skb->len;
	is_eapol = (skb->protocol == htons(ETH_P_PAE));

	/* Create space for payload header */
	pad_len = sizeof(struct esp_payload_header);

	total_len = len + pad_len;

	/* Align buffer length */
	pad_len += (SKB_DATA_ADDR_ALIGNMENT - (total_len % SKB_DATA_ADDR_ALIGNMENT)) %
		SKB_DATA_ADDR_ALIGNMENT;

	if (skb_headroom(skb) < pad_len) {
		/* Headroom is not sufficient */
		realloc_skb = 1;
	}

	if (realloc_skb || !IS_ALIGNED((unsigned long) skb->data, SKB_DATA_ADDR_ALIGNMENT)) {
		/* Realloc SKB */
		if (skb_linearize(skb)) {
			priv->stats.tx_errors++;
			dev_kfree_skb(skb);
			esp_err("Failed to linearize SKB");
			return NETDEV_TX_OK;
		}

		new_skb = esp_if_alloc_skb(priv->adapter, skb->len + pad_len);

		if (!new_skb) {
			esp_err("Failed to allocate SKB");
			priv->stats.tx_errors++;
			dev_kfree_skb(skb);
			return NETDEV_TX_OK;
		}

		pos = new_skb->data;
		pos += pad_len;

		/* Populate new SKB */
		skb_copy_from_linear_data(skb, pos, skb->len);
		skb_put(new_skb, skb->len + pad_len);
		new_skb->protocol = skb->protocol;
		((struct esp_skb_cb *)new_skb->cb)->priv = priv;

		/* Replace old SKB */
		dev_kfree_skb_any(skb);
		skb = new_skb;
	} else {
		/* Realloc is not needed, Make space for interface header */
		skb_push(skb, pad_len);
	}

	/* Set payload header */
	payload_header = (struct esp_payload_header *) skb->data;
	memset(payload_header, 0, pad_len);

	payload_header->if_type = priv->if_type;
	payload_header->if_num = priv->if_num;
	payload_header->len = esp_wire_cpu_to_le16(len);
	payload_header->offset = esp_wire_cpu_to_le16(pad_len);
	if (is_eapol)
		payload_header->packet_type = PACKET_TYPE_EAPOL;
	else
		payload_header->packet_type = PACKET_TYPE_DATA;

	if (adapter.capabilities & ESP_CHECKSUM_ENABLED)
		payload_header->checksum = esp_wire_cpu_to_le16(compute_checksum(skb->data,
									   len + pad_len));

	if (!priv->stop_data && (priv->port_open || is_eapol)) {
		/* esp_send_packet() consumes skb on every return path. */
		ret = esp_send_packet(priv->adapter, skb);

		if (ret) {
			esp_verbose("Failed to send SKB");
			priv->stats.tx_errors++;
		} else {
			priv->stats.tx_packets++;
			priv->stats.tx_bytes += len;
		}
	} else {
		dev_kfree_skb_any(skb);
		priv->stats.tx_dropped++;
	}

	return 0;
}

void esp_port_open(struct esp_wifi_device *priv)
{
	if (!priv)
		return;

	priv->port_open = 1;
	priv->stop_data = 0;
}

void esp_port_close(struct esp_wifi_device *priv)
{
	if (!priv)
		return;

	priv->port_open = 0;
}

void print_capabilities(u32 cap)
{
	esp_info("Capabilities: 0x%x. Features supported are:\n", cap);
	if (cap & ESP_WLAN_SDIO_SUPPORT)
		esp_info("\t * WLAN on SDIO\n");
	else if (cap & ESP_WLAN_SPI_SUPPORT)
		esp_info("\t * WLAN on SPI\n");

	if ((cap & ESP_BT_UART_SUPPORT) ||
		    (cap & ESP_BT_SDIO_SUPPORT) ||
		    (cap & ESP_BT_SPI_SUPPORT)) {
		esp_info("\t * BT/BLE\n");
		if (cap & ESP_BT_UART_SUPPORT)
			esp_info("\t   - HCI over UART\n");
		if (cap & ESP_BT_SDIO_SUPPORT)
			esp_info("\t   - HCI over SDIO\n");
		if (cap & ESP_BT_SPI_SUPPORT)
			esp_info("\t   - HCI over SPI\n");

		if ((cap & ESP_BLE_ONLY_SUPPORT) && (cap & ESP_BR_EDR_ONLY_SUPPORT))
			esp_info("\t   - BT/BLE dual mode\n");
		else if (cap & ESP_BLE_ONLY_SUPPORT)
			esp_info("\t   - BLE only\n");
		else if (cap & ESP_BR_EDR_ONLY_SUPPORT)
			esp_info("\t   - BR EDR only\n");
	}
}

static int init_bt(struct esp_adapter *adapter)
{
	if ((adapter->capabilities & ESP_BT_SPI_SUPPORT) ||
	    (adapter->capabilities & ESP_BT_SDIO_SUPPORT)) {
		msleep(200);
		esp_info("ESP Bluetooth init\n");
		return esp_init_bt(adapter);
	}
	return 0;
}

static int check_esp_version(struct fw_version *ver)
{
	snprintf(version_str, ESP_VERSION_BUFFER_SIZE, "%.*s-%u.%u.%u.%u.%u",
		(int)sizeof(ver->project_name), ver->project_name,
		ver->major1, ver->major2, ver->minor,
		ver->revision_patch_1, ver->revision_patch_2);

	if (strcmp(RELEASE_VERSION, version_str) != 0) {
		esp_err("Firmware version: %s, Host version: %s\n", version_str, RELEASE_VERSION);
		esp_err("Incompatible ESP Host-firmware release detected, Please use correct ESP-Hosted branch/compatible release\n");
		return -1;
	}
	esp_info("ESP-Hosted Version: %s\n", version_str);
	return 0;
}

static void print_reset_reason(uint32_t reason)
{
	switch (reason)
	{
		case 1: esp_info("POWERON_RESET\n"); break;
		case 3: esp_info("SW_RESET\n"); break;
		case 4: esp_info("OWDT_RESET\n"); break;
		case 5: esp_info("DEEPSLEEP_RESET\n"); break;
		case 6: esp_info("SDIO_RESET\n"); break;
		case 7: esp_info("TG0WDT_SYS_RESET\n"); break;
		case 8: esp_info("TG1WDT_SYS_RESET\n"); break;
		case 9: esp_info("RTCWDT_SYS_RESET\n"); break;
		case 10: esp_info("INTRUSION_RESET\n"); break;
		case 11: esp_info("TGWDT_CPU_RESET\n"); break;
		case 12: esp_info("SW_CPU_RESET\n"); break;
		case 13: esp_info("RTCWDT_CPU_RESET\n"); break;
		case 14: esp_info("EXT_CPU_RESET\n"); break;
		case 15: esp_info("RTCWDT_BROWN_OUT_RESET\n"); break;
		case 16: esp_info("RTCWDT_RTC_RESET\n"); break;
		default: esp_info("Unknown[%u]\n", reason); break;
	}
}

static int process_fw_data(struct fw_data *fw_p, int tag_len)
{
	if (tag_len != sizeof(struct fw_data)) {
		esp_err("Length not matching to firmware data size\n");
		return -1;
	}

	esp_info("ESP chipset's last reset cause:\n");
	print_reset_reason(esp_wire_le32_to_cpu(fw_p->last_reset_reason));

	return check_esp_version(&fw_p->version);
}

static void esp_begin_reconstruction(struct esp_adapter *adapter)
{
	unsigned long flags;

	if (!adapter)
		return;

	spin_lock_irqsave(&adapter->fw_recovery_lock, flags);
	set_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags);
	adapter->fw_reconstruct_gen = atomic_read(&adapter->fw_recovery_gen);
	spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);
}

static void esp_clear_reconstruct_flag(struct esp_adapter *adapter)
{
	unsigned long flags;

	if (!adapter)
		return;

	spin_lock_irqsave(&adapter->fw_recovery_lock, flags);
	clear_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags);
	spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);
}

static void esp_commit_reconstruction(struct esp_adapter *adapter)
{
	unsigned long flags;
	u32 gen;
	bool need_recovery = false;

	if (!adapter)
		return;

	/* INIT_DONE first so ordinary commands stay gated by ALLOW_RECONSTRUCT
	 * until that bit is cleared under the recovery lock. */
	set_bit(ESP_INIT_DONE, &adapter->state_flags);

	/* Must not hold fw_recovery_lock: the worker may call schedule_recovery. */
	cancel_delayed_work_sync(&adapter->fw_recovery_work);

	spin_lock_irqsave(&adapter->fw_recovery_lock, flags);
	clear_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags);
	gen = atomic_read(&adapter->fw_recovery_gen);
	if (gen != adapter->fw_reconstruct_gen) {
		need_recovery = true;
		atomic_set(&adapter->state, ESP_CONTEXT_RX_READY);
	} else {
		clear_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags);
		clear_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags);
		clear_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);
		atomic_set(&adapter->state, ESP_CONTEXT_READY);
	}
	spin_unlock_irqrestore(&adapter->fw_recovery_lock, flags);

	if (need_recovery) {
		esp_info("Transport recovery: request arrived during reconstruction\n");
		schedule_delayed_work(&adapter->fw_recovery_work,
				      msecs_to_jiffies(ESP_FW_RECOVERY_QUIET_MS));
	}
}

static int process_event_esp_bootup(struct esp_adapter *adapter, u8 *evt_buf, u8 len)
{
	int len_left = len, tag_len, ret = 0;
	u8 *pos;
	struct fw_data *fw_p;
	bool reinitializing = false;
	bool committed = false;

	if (!adapter || !evt_buf)
		return -1;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return -ESHUTDOWN;

	cancel_delayed_work_sync(&adapter->fw_recovery_work);

	if (len_left >= 64) {
		esp_info("ESP init event len looks unexpected: %u (>=64)\n", len_left);
		esp_info("You probably facing timing mismatch at transport layer\n");
	}

	reinitializing = test_bit(ESP_INIT_DONE, &adapter->state_flags) ||
		test_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags) ||
		test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags);
	if (reinitializing)
		set_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags);
	clear_bit(ESP_INIT_DONE, &adapter->state_flags);
	clear_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);
	adapter->tx_aggr_size = 0;

	if (reinitializing) {
		set_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
		skb_queue_purge(&adapter->events_skb_q);
		test_raw_tp_cleanup();
		if (adapter->if_ops && adapter->if_ops->quiesce_for_fw_reset) {
			ret = adapter->if_ops->quiesce_for_fw_reset(adapter);
			if (ret)
				goto fail;
		}
		ret = esp_remove_card(adapter, false);
		if (ret)
			goto fail;
		if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
		    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
			return -ESHUTDOWN;
	}

	pos = evt_buf;

	while (len_left > 0) {
		if (len_left < 2) {
			ret = -EINVAL;
			goto fail;
		}
		tag_len = *(pos + 1);
		if (tag_len > len_left - 2) {
			ret = -EINVAL;
			goto fail;
		}

		esp_info("Boot-up Event tag: %d\n", *pos);

		switch (*pos) {
		case ESP_BOOTUP_CAPABILITY:
			if (tag_len != 1) {
				ret = -EINVAL;
				goto fail;
			}
			adapter->capabilities = *(pos + 2);
			break;
		case ESP_BOOTUP_RX_BUF_SIZE:
			if (tag_len != sizeof(u32)) {
				ret = -EINVAL;
				goto fail;
			}
			adapter->tx_aggr_size = le32_to_cpup((__le32 *)(pos + 2));
			if (!adapter->tx_aggr_size ||
			    adapter->tx_aggr_size > ESP_TX_AGGR_SIZE_MAX ||
			    adapter->tx_aggr_size % ESP_TX_AGGR_SIZE_ALIGN) {
				esp_err("Invalid slave RX aggregate size: %u\n",
					adapter->tx_aggr_size);
				ret = -EINVAL;
				goto fail;
			}
			esp_info("Slave RX Buffer Size configured dynamically: %u bytes\n",
				 adapter->tx_aggr_size);
			break;
		case ESP_BOOTUP_FIRMWARE_CHIP_ID:
			if (tag_len != 1) {
				ret = -EINVAL;
				goto fail;
			}
			ret = esp_validate_chipset(adapter, *(pos + 2));
			break;
		case ESP_BOOTUP_FW_DATA:
			if (tag_len != sizeof(struct fw_data)) {
				ret = -EINVAL;
				goto fail;
			}
			fw_p = (struct fw_data *)(pos + 2);
			ret = process_fw_data(fw_p, tag_len);
			break;
		case ESP_BOOTUP_SPI_CLK_MHZ:
			if (tag_len != 1) {
				ret = -EINVAL;
				goto fail;
			}
			ret = esp_adjust_spi_clock(adapter, *(pos + 2));
			break;
		default:
			esp_warn("Unsupported tag=%x in boot-up event\n", *pos);
		}

		if (ret < 0) {
			esp_err("failed to process tag=%x in boot-up event\n", *pos);
			goto fail;
		}
		pos += (tag_len + 2);
		len_left -= (tag_len + 2);
	}

	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return -ESHUTDOWN;

	if (reinitializing && adapter->if_ops &&
	    adapter->if_ops->reinit_after_fw_reset) {
		ret = adapter->if_ops->reinit_after_fw_reset(adapter);
		if (ret)
			goto fail;
	}

	esp_begin_reconstruction(adapter);

	if (adapter->capabilities & ESP_WLAN_SDIO_SUPPORT ||
	    adapter->capabilities & ESP_BT_SDIO_SUPPORT)
		atomic_set(&adapter->state, ESP_CONTEXT_READY);

	ret = esp_add_card(adapter);
	if (ret) {
		esp_err("network interface init failed\n");
		goto fail;
	}

	if (ota_file && strlen(ota_file) != 0) {
		esp_info("OTA update requested: bin(%s)\n", ota_file);
		ret = esp_start_ota(adapter, ota_file);
		ota_file = NULL;
		if (ret) {
			esp_err("OTA update failed: %d\n", ret);
			/* A submitted OTA command timeout publishes reset quarantine.
			 * Never clear it by committing this same firmware incarnation. */
			if (test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags))
				goto fail;
		} else {
			esp_clear_reconstruct_flag(adapter);
			esp_schedule_recovery(adapter, ESP_OTA_RECOVERY_DELAY_MS,
					     true, true);
			return 0;
		}
	}

	if (raw_tp_mode != 0) {
#if TEST_RAW_TP
		/* Firmware setup is a constituent of reconstruction. The host
		 * producer/timer is published only after reconstruction succeeds. */
		ret = esp_init_raw_tp(adapter);
		if (ret)
			goto fail;
#else
		esp_err("RAW TP mode selected but not enabled\n");
		ret = -1;
		goto fail;
#endif
	}

	esp_commit_reconstruction(adapter);
	committed = true;

	ret = init_bt(adapter);
	if (ret) {
		esp_err("Bluetooth init failed: %d\n", ret);
		goto fail_after_commit;
	}

	ret = esp_publish_network_ifaces(adapter);
	if (ret) {
		esp_err("network interface publish failed\n");
		goto fail_after_commit;
	}

#if TEST_RAW_TP
	if (raw_tp_mode != 0)
		process_test_capabilities(raw_tp_mode);
#endif
	print_capabilities(adapter->capabilities);
	return 0;

fail_after_commit:
	if (committed) {
		set_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
		clear_bit(ESP_INIT_DONE, &adapter->state_flags);
		esp_deinit_bt(adapter);
		esp_remove_card(adapter, true);
	}
fail:
	esp_clear_reconstruct_flag(adapter);
	esp_bootup_request_retry(adapter);
	return ret < 0 ? ret : -1;
}

static int esp_open(struct net_device *ndev)
{
	struct esp_wifi_device *priv = netdev_priv(ndev);

	if (!priv)
		return -EINVAL;

	if (!test_bit(ESP_INTERFACE_INITIALIZED, &priv->priv_flags)) {
		if (cmd_init_interface(priv) != 0)
			return -EINVAL;
		set_bit(ESP_INTERFACE_INITIALIZED, &priv->priv_flags);
	}

	return 0;
}

static int esp_stop(struct net_device *ndev)
{
	struct esp_wifi_device *priv = netdev_priv(ndev);

	if (!priv)
		return 0;

	esp_mark_scan_done_and_disconnect(priv, false);
	esp_port_close(priv);
	priv->stop_data = 1;
	if (test_bit(ESP_INTERFACE_INITIALIZED, &priv->priv_flags)) {
		if (!test_bit(ESP_SKIP_FW_DEINIT, &priv->adapter->state_flags)) {
			esp_info("Deinitializing interface %s on ESP side\n", ndev->name);
			if (cmd_deinit_interface(priv) != 0) {
				esp_err("Failed to deinit interface");
				return 0;
			}
		}
		clear_bit(ESP_INTERFACE_INITIALIZED, &priv->priv_flags);
	}
	return 0;
}

static struct net_device_stats *esp_get_stats(struct net_device *ndev)
{
	struct esp_wifi_device *priv = netdev_priv(ndev);

	if (!priv)
		return NULL;

	return &priv->stats;
}

static int esp_set_mac_address(struct net_device *ndev, void *data)
{
	struct esp_wifi_device *priv = netdev_priv(ndev);
	struct sockaddr *sa = (struct sockaddr *)data;
	int ret;

	if (!priv || !priv->adapter)
		return -EINVAL;

	esp_info("%u "MACSTR"\n", __LINE__, MAC2STR(sa->sa_data));
	ret = cmd_set_mac(priv, sa->sa_data);
	if (ret == 0)
		ETH_HW_ADDR_SET(ndev, priv->mac_address);
	return ret;
}

static void esp_set_rx_mode(struct net_device *ndev)
{
	struct esp_adapter *adapter = esp_get_adapter();

	if (!adapter ||
	    test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) ||
	    test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return;
	schedule_work(&adapter->mac_flter_work);
}

static int esp_hard_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct esp_wifi_device *priv = NULL;
	struct esp_skb_cb *cb = NULL;
	bool is_eapol;

	if (!skb || !ndev)
		return NETDEV_TX_OK;

	priv = netdev_priv(ndev);
	if (!priv) {
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	is_eapol = (skb->protocol == htons(ETH_P_PAE));
	if (!priv->port_open && !is_eapol) {
		priv->stats.tx_dropped++;
		esp_verbose("Port not yet open\n");
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	if (!skb->len || (skb->len > ETH_FRAME_LEN)) {
		esp_err("Bad len %d\n", skb->len);
		priv->stats.tx_dropped++;
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	cb = (struct esp_skb_cb *) skb->cb;
	cb->priv = priv;

	return process_tx_packet(skb);
}

static const struct net_device_ops esp_netdev_ops = {
	.ndo_open = esp_open,
	.ndo_stop = esp_stop,
	.ndo_start_xmit = esp_hard_start_xmit,
	.ndo_set_mac_address = esp_set_mac_address,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_get_stats = esp_get_stats,
	.ndo_set_rx_mode = esp_set_rx_mode,
};

void esp_init_priv(struct net_device *ndev)
{
	ndev->netdev_ops = &esp_netdev_ops;
	ndev->needed_headroom = roundup(sizeof(struct esp_payload_header) +
			INTERFACE_HEADER_PADDING, 4);
}

static int esp_add_network_ifaces(struct esp_adapter *adapter)
{
	struct wireless_dev *wdev = NULL;

	if (!adapter) {
		esp_info("adapter not yet init\n");
		return -EINVAL;
	}

	rtnl_lock();
	wdev = esp_cfg80211_add_iface(adapter->wiphy, "wlan%d", 1, NL80211_IFTYPE_STATION, NULL);
	rtnl_unlock();

	if (IS_ERR(wdev))
		return PTR_ERR(wdev);
	if (!wdev)
		return -EIO;
	return 0;
}

static int esp_publish_network_ifaces(struct esp_adapter *adapter)
{
	uint8_t iface_idx;
	int ret = 0;

	if (!adapter)
		return -EINVAL;

	rtnl_lock();
	for (iface_idx = 0; iface_idx < ESP_MAX_INTERFACE; iface_idx++) {
		if (!adapter->priv[iface_idx])
			continue;
		ret = esp_cfg80211_register_iface(adapter->priv[iface_idx]);
		if (ret)
			break;
	}
	rtnl_unlock();
	return ret;
}

int esp_start_ota(struct esp_adapter *adapter, char *ota_file)
{
	struct file *file;
	ssize_t nread;
	int ret = 0;
	char *ota_chunk = kmalloc(OTA_CHUNK_SIZE, GFP_KERNEL);

	if (!ota_chunk) {
		esp_err("Failed to allocate buffer for ota_chunk\n");
		return -ENOMEM;
	}

	memset(ota_chunk, 0, OTA_CHUNK_SIZE);
	file = filp_open(ota_file, O_RDONLY, 0);
	if (IS_ERR(file)) {
		esp_err("Error reading ota bin, or ota bin not found at %s \n", ota_file);
		kfree(ota_chunk);
		return -EINVAL;
	}

	set_bit(ESP_OTA_IN_PROGRESS, &adapter->state_flags);
	ret = cmd_process_ota_start(adapter->priv[ESP_STA_NW_IF]);
	if (ret) {
		esp_err("OTA Start failed: %d\n", ret);
		goto done;
	}

	while ((nread = esp_kernel_read(file, ota_chunk, OTA_CHUNK_SIZE, &file->f_pos)) > 0) {
		ret = cmd_process_ota_write(adapter->priv[ESP_STA_NW_IF], ota_chunk, nread);
		if (ret) {
			esp_err("OTA Write failed: %d\n", ret);
			goto done;
		}
		if (nread < OTA_CHUNK_SIZE)
			break;
	}

	if (nread < 0) {
		esp_err("Failed to read ota binary file %s \n", ota_file);
		ret = (int)nread;
		esp_ota_finish_or_recover(adapter->priv[ESP_STA_NW_IF], ret);
		goto done;
	}

	ret = cmd_process_ota_end(adapter->priv[ESP_STA_NW_IF]);
	if (ret)
		esp_err("cmd_process_ota_end failed %d \n", ret);

done:
	kfree(ota_chunk);
	filp_close(file, NULL);
	clear_bit(ESP_OTA_IN_PROGRESS, &adapter->state_flags);
	return ret;
}

int esp_init_raw_tp(struct esp_adapter *adapter)
{
	RET_ON_FAIL(cmd_init_raw_tp_task_timer(adapter->priv[ESP_STA_NW_IF]));
	return 0;
}

int esp_add_card(struct esp_adapter *adapter)
{
	int ret;

	if (!adapter)
		return -EINVAL;

	ret = esp_commands_setup(adapter);
	if (ret)
		return ret;

	ret = esp_add_wiphy(adapter);
	if (ret)
		goto rollback_commands;

	ret = esp_add_network_ifaces(adapter);
	if (ret)
		goto rollback_wiphy;

	clear_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
	return 0;

rollback_wiphy:
	esp_remove_wiphy(adapter);
rollback_commands:
	esp_commands_teardown(adapter);
	return ret;
}

static int esp_remove_network_ifaces(struct esp_adapter *adapter)
{
	uint8_t iface_idx = 0;
	struct net_device *ndev = NULL;
	struct esp_wifi_device *priv = NULL;

	for (iface_idx = 0; iface_idx < ESP_MAX_INTERFACE; iface_idx++) {
		priv = adapter->priv[iface_idx];
		if (!priv)
			continue;
		if (!test_bit(ESP_NETWORK_UP, &priv->priv_flags)) {
			ndev = priv->ndev;
			adapter->priv[iface_idx] = NULL;
			esp_mlme_cancel(priv);
			if (ndev) {
				dev_net_set(ndev, NULL);
				free_netdev(ndev);
			}
			continue;
		}

		ndev = priv->ndev;
		esp_mlme_cancel(priv);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
		if (ndev)
			ndev->needs_free_netdev = true;
		rtnl_lock();
		wiphy_lock(adapter->wiphy);
		cfg80211_unregister_wdev(&priv->wdev);
		wiphy_unlock(adapter->wiphy);
		rtnl_unlock();
#else
		if (ndev && ndev->reg_state == NETREG_REGISTERED) {
			unregister_netdev(ndev);
			free_netdev(ndev);
			ndev = NULL;
		}
#endif
		adapter->priv[iface_idx] = NULL;
	}

	return 0;
}

static int stop_network_iface(struct esp_wifi_device *priv)
{
	struct net_device *ndev;

	if (!priv)
		return 0;

	if (!test_bit(ESP_NETWORK_UP, &priv->priv_flags)) {
		if (test_bit(ESP_INTERFACE_INITIALIZED, &priv->priv_flags) &&
		    priv->adapter &&
		    !test_bit(ESP_SKIP_FW_DEINIT, &priv->adapter->state_flags))
			cmd_deinit_interface(priv);
		return 0;
	}

	esp_mark_scan_done_and_disconnect(priv, false);
	esp_port_close(priv);
	ndev = priv->ndev;
	if (ndev) {
		netif_carrier_off(ndev);
		netif_device_detach(ndev);
		unregister_inetaddr_notifier(&(priv->nb));
	}
	return 0;
}

static int esp_stop_network_ifaces(struct esp_adapter *adapter)
{
	uint8_t iface_idx = 0;

	for (iface_idx = 0; iface_idx < ESP_MAX_INTERFACE; iface_idx++)
		stop_network_iface(adapter->priv[iface_idx]);

	rtnl_lock();
	if (adapter->wiphy)
		cfg80211_shutdown_all_interfaces(adapter->wiphy);
	rtnl_unlock();
	return 0;
}

int esp_remove_card(struct esp_adapter *adapter, bool notify_fw)
{
	if (!adapter)
		return 0;

	esp_cmd_abort_waiters(adapter);
	cancel_work_sync(&adapter->mac_flter_work);

	if (!notify_fw)
		set_bit(ESP_SKIP_FW_DEINIT, &adapter->state_flags);
	esp_stop_network_ifaces(adapter);
	if (!notify_fw)
		clear_bit(ESP_SKIP_FW_DEINIT, &adapter->state_flags);

	esp_cfg_cleanup(adapter);
	if (adapter->if_rx_workqueue)
		flush_workqueue(adapter->if_rx_workqueue);
	/* BT may have been initialized after fw boot-up event, deinit it */
	esp_deinit_bt(adapter);
	esp_commands_teardown(adapter);
	esp_remove_network_ifaces(adapter);
	esp_remove_wiphy(adapter);
	return 0;
}

struct esp_wifi_device *get_priv_from_payload_header(struct esp_payload_header *header)
{
	struct esp_wifi_device *priv = NULL;
	u8 i = 0;

	if (!header)
		return NULL;

	for (i = 0; i < ESP_MAX_INTERFACE; i++) {
		priv = adapter.priv[i];
		if (!priv) {
			esp_err("dropping pkt, driver not initialized\n");
			continue;
		}
		if (priv->if_num == header->if_num)
			return priv;
		esp_err("dropping pkt, priv iftype=%d ifnum=%d, header iftype=%d ifnum=%d\n",
			priv->if_type, priv->if_num, header->if_type, header->if_num);
	}
	return NULL;
}

static void process_esp_bootup_event(struct esp_adapter *adapter,
		struct sk_buff *skb)
{
	struct esp_internal_bootup_event *evt;

	if (!adapter || !skb) {
		esp_err("Invalid arguments\n");
		return;
	}
	if (skb->len < offsetof(struct esp_internal_bootup_event, data)) {
		esp_err("Boot-up event truncated skb_len=%u\n", skb->len);
		return;
	}

	evt = (struct esp_internal_bootup_event *)skb->data;
	if (evt->header.status) {
		esp_err("Incorrect ESP boot-up event\n");
		return;
	}
	if (evt->len > skb->len - offsetof(struct esp_internal_bootup_event, data)) {
		esp_err("Boot-up event TLV len=%u exceeds skb_len=%u\n",
			evt->len, skb->len);
		return;
	}

	esp_info("Received ESP boot-up event\n");
	process_event_esp_bootup(adapter, evt->data, evt->len);
}

static int process_internal_event(struct esp_adapter *adapter,
		struct sk_buff *skb)
{
	struct event_header *header = NULL;

	if (!skb || !adapter) {
		esp_err("Incorrect event data!\n");
		return -1;
	}
	if (skb->len < sizeof(*header)) {
		esp_err("Internal event truncated skb_len=%u\n", skb->len);
		return -EMSGSIZE;
	}

	header = (struct event_header *)skb->data;
	switch (header->event_code) {
	case ESP_INTERNAL_BOOTUP_EVENT:
		process_esp_bootup_event(adapter, skb);
		break;
	default:
		esp_info("%u unhandled internal event[%u]\n", __LINE__, header->event_code);
		break;
	}
	return 0;
}

static void process_rx_packet(struct esp_adapter *adapter, struct sk_buff *skb)
{
	struct esp_wifi_device *priv = NULL;
	struct esp_payload_header *payload_header = NULL;
	u16 len = 0, offset = 0;
	u16 rx_checksum = 0, checksum = 0;
	struct hci_dev *hdev = adapter->hcidev;

	if (!skb)
		return;
	if (skb->len < sizeof(*payload_header)) {
		esp_err("RX drop: runt transport frame skb_len=%u header=%zu\n",
			skb->len, sizeof(*payload_header));
		dev_kfree_skb_any(skb);
		return;
	}

	payload_header = (struct esp_payload_header *)skb->data;
	len = esp_wire_le16_to_cpu(payload_header->len);
	offset = esp_wire_le16_to_cpu(payload_header->offset);
	if (!ESP_OFFSET_VALID(offset) || offset > skb->len || len > skb->len - offset) {
		esp_err("RX drop: invalid bounds if=%u pkt=%u len=%u offset=%u skb_len=%u\n",
			payload_header->if_type, payload_header->packet_type,
			len, offset, skb->len);
		dev_kfree_skb_any(skb);
		return;
	}

	if (payload_header->if_type != ESP_TEST_IF && payload_header->reserved2 == 0xFF)
		esp_hex_dump("Wake up packet: ", skb->data, len + offset);

	if (adapter->capabilities & ESP_CHECKSUM_ENABLED) {
		rx_checksum = esp_wire_le16_to_cpu(payload_header->checksum);
		payload_header->checksum = 0;
		checksum = compute_checksum(skb->data, len + offset);
		if (checksum != rx_checksum) {
			esp_err("ESP_RX_CHECKSUM_ERROR: if=%u pkt=%u len=%u offset=%u skb_len=%u expected=0x%04x got=0x%04x\n",
				payload_header->if_type, payload_header->packet_type,
				len, offset, skb->len, checksum, rx_checksum);
			dev_kfree_skb_any(skb);
			return;
		}
	}

	if (payload_header->if_type != ESP_INTERNAL_IF &&
	    atomic_read(&adapter->state) < ESP_CONTEXT_READY) {
		dev_kfree_skb_any(skb);
		return;
	}
	if (payload_header->if_type != ESP_INTERNAL_IF &&
	    test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) &&
	    !test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags)) {
		dev_kfree_skb_any(skb);
		return;
	}

	skb_pull(skb, offset);

	if (payload_header->if_type == ESP_STA_IF || payload_header->if_type == ESP_AP_IF) {
		priv = get_priv_from_payload_header(payload_header);
		if (!priv) {
			dev_kfree_skb_any(skb);
			return;
		}

		if (payload_header->packet_type == PACKET_TYPE_EAPOL) {
			if (!priv->ndev || !test_bit(ESP_NETWORK_UP, &priv->priv_flags) ||
			    priv->ndev->reg_state != NETREG_REGISTERED) {
				dev_kfree_skb_any(skb);
				return;
			}
			skb->dev = priv->ndev;
			skb->protocol = eth_type_trans(skb, priv->ndev);
			netif_rx(skb);
		} else if (payload_header->packet_type == PACKET_TYPE_DATA) {
			if (!priv->ndev || !test_bit(ESP_NETWORK_UP, &priv->priv_flags) ||
			    priv->ndev->reg_state != NETREG_REGISTERED) {
				dev_kfree_skb_any(skb);
				return;
			}
			skb->dev = priv->ndev;
			skb->protocol = eth_type_trans(skb, priv->ndev);
			skb->ip_summed = CHECKSUM_NONE;
			priv->stats.rx_bytes += skb->len;
			NETIF_RX_NI(skb);
			priv->stats.rx_packets++;
		} else if (payload_header->packet_type == PACKET_TYPE_COMMAND_RESPONSE) {
			process_cmd_resp(priv->adapter, skb);
		} else if (payload_header->packet_type == PACKET_TYPE_EVENT) {
			process_cmd_event(priv, skb);
			dev_kfree_skb_any(skb);
		} else {
			dev_kfree_skb_any(skb);
		}
	} else if (payload_header->if_type == ESP_HCI_IF) {
		u8 pkt_type;
		u16 payload_len;

		if (!hdev || test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
		    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
		    !test_bit(ESP_INIT_DONE, &adapter->state_flags) ||
		    test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags)) {
			dev_kfree_skb_any(skb);
			return;
		}
		if (!skb->len) {
			dev_kfree_skb_any(skb);
			return;
		}
		if (skb->len > len)
			skb_trim(skb, len);
		pkt_type = skb->data[0];
		payload_len = skb->len - 1;
		hci_skb_pkt_type(skb) = pkt_type;
		skb_pull(skb, 1);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0))
		if (hci_recv_frame(hdev, skb))
#else
		if (hci_recv_frame(skb))
#endif
			hdev->stat.err_rx++;
		else
			esp_hci_update_rx_counter(hdev, pkt_type, payload_len);
	} else if (payload_header->if_type == ESP_INTERNAL_IF) {
		if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
		    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags)) {
			dev_kfree_skb_any(skb);
			return;
		}
		skb_queue_tail(&adapter->events_skb_q, skb);
		if (adapter->events_wq)
			queue_work(adapter->events_wq, &adapter->events_work);
		else
			dev_kfree_skb_any(skb);
	} else if (payload_header->if_type == ESP_TEST_IF) {
#if TEST_RAW_TP
		if (raw_tp_mode != 0)
			update_test_raw_tp_rx_stats(payload_header, len);
#endif
		dev_kfree_skb_any(skb);
	} else {
		dev_kfree_skb_any(skb);
	}
}

char *esp_get_hardware_name(int hardware_id)
{
	if(hardware_id == ESP_FIRMWARE_CHIP_ESP32)
		return "ESP32";
	else if(hardware_id == ESP_FIRMWARE_CHIP_ESP32S2)
		return "ESP32S2";
	else if(hardware_id == ESP_FIRMWARE_CHIP_ESP32C3)
		return "ESP32C3";
	else if(hardware_id == ESP_FIRMWARE_CHIP_ESP32S3)
		return "ESP32S3";
	else if(hardware_id == ESP_FIRMWARE_CHIP_ESP32C2)
		return "ESP32C2";
	else if(hardware_id == ESP_FIRMWARE_CHIP_ESP32C6)
		return "ESP32C6";
	else if(hardware_id == ESP_FIRMWARE_CHIP_ESP32C61)
		return "ESP32C61";
	else if(hardware_id == ESP_FIRMWARE_CHIP_ESP32C5)
		return "ESP32C5";
	else
		return "N/A";
}

bool esp_is_valid_hardware_id(int hardware_id)
{
	switch(hardware_id) {
	case ESP_FIRMWARE_CHIP_ESP32:
	case ESP_FIRMWARE_CHIP_ESP32S2:
	case ESP_FIRMWARE_CHIP_ESP32C3:
	case ESP_FIRMWARE_CHIP_ESP32S3:
	case ESP_FIRMWARE_CHIP_ESP32C2:
	case ESP_FIRMWARE_CHIP_ESP32C6:
	case ESP_FIRMWARE_CHIP_ESP32C61:
	case ESP_FIRMWARE_CHIP_ESP32C5:
		return true;
	default:
		return false;
	}
}

int esp_is_tx_queue_paused(struct esp_wifi_device *priv)
{
	if (!priv || !priv->ndev)
		return 0;
	if (!netif_queue_stopped((const struct net_device *)priv->ndev))
		return 1;
	return 0;
}

void esp_tx_pause(struct esp_wifi_device *priv)
{
	if (!priv || !priv->ndev)
		return;
	if (!netif_queue_stopped((const struct net_device *)priv->ndev))
		netif_stop_queue(priv->ndev);
}

void esp_tx_resume(struct esp_wifi_device *priv)
{
	if (!priv || !priv->ndev)
		return;
	if (netif_queue_stopped((const struct net_device *)priv->ndev))
		netif_wake_queue(priv->ndev);
}

static int esp_get_packets(struct esp_adapter *adapter)
{
	struct sk_buff *skb = NULL;

	if (!adapter || !adapter->if_ops || !adapter->if_ops->read)
		return -EINVAL;
	while ((skb = adapter->if_ops->read(adapter)))
		process_rx_packet(adapter, skb);
	return 0;
}

int esp_send_packet(struct esp_adapter *adapter, struct sk_buff *skb)
{
	if (!skb)
		return -EINVAL;
	if (!adapter || !adapter->if_ops || !adapter->if_ops->write) {
		esp_err("%u adapter: %p\n", __LINE__, adapter);
		dev_kfree_skb_any(skb);
		return -EINVAL;
	}
	if (test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
	    (test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags))) {
		dev_kfree_skb_any(skb);
		return -ESHUTDOWN;
	}
	return adapter->if_ops->write(adapter, skb);
}

static void esp_if_rx_work(struct work_struct *work)
{
	esp_get_packets(&adapter);
}

static void update_mac_filter(struct work_struct *work)
{
	struct esp_adapter *adapter = esp_get_adapter();
	struct esp_wifi_device *priv;
	struct net_device *ndev;
	struct netdev_hw_addr *mac_addr;
	u32 count = 0;
	int ret = 0;
	int retry;

	if (!adapter || test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) ||
	    test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return;

	priv = adapter->priv[0];
	if (!priv)
		return;
	ndev = priv->ndev;
	if (!ndev)
		return;
	if (!priv->port_open) {
		esp_verbose("Port is not open yet, skipping mac filter update\n");
		return;
	}

#if CONFIG_ALLOW_MULTICAST_WAKEUP
	netdev_for_each_mc_addr(mac_addr, ndev) {
		if (count < MAX_MULTICAST_ADDR_COUNT) {
			esp_verbose("%d: "MACSTR"\n", count+1, MAC2STR(mac_addr->addr));
			memcpy(&mcast_list.mcast_addr[count++], mac_addr->addr, ETH_ALEN);
		}
	}
	mcast_list.priv = priv;
	mcast_list.addr_count = count;

	for (retry = 0; retry < 3; retry++) {
		ret = cmd_set_mcast_mac_list(mcast_list.priv, &mcast_list);
		if (!ret)
			break;
		if (test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) ||
		    test_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags) ||
		    test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags))
			break;
		msleep(20U << retry);
	}
	if (ret)
		esp_warn("Failed to update firmware multicast/WoW filter: %d\n", ret);
#else
	esp_info("Not setting FW multicast addresses\n");
#endif
}

static void esp_events_work(struct work_struct *work)
{
	struct sk_buff *skb = NULL;

	while ((skb = skb_dequeue(&adapter.events_skb_q)) != NULL) {
		if (!test_bit(ESP_DRIVER_UNLOADING, &adapter.state_flags) &&
		    !test_bit(ESP_TRANSPORT_REMOVING, &adapter.state_flags) && skb->data)
			process_internal_event(&adapter, skb);
		dev_kfree_skb_any(skb);
	}
}

static struct esp_adapter *init_adapter(void)
{
	memset(&adapter, 0, sizeof(adapter));
	skb_queue_head_init(&adapter.events_skb_q);
	INIT_DELAYED_WORK(&adapter.fw_recovery_work, esp_fw_recovery_work);
	spin_lock_init(&adapter.fw_recovery_lock);
	adapter.fw_recovery_backoff_ms = ESP_FW_RECOVERY_QUIET_MS;

	adapter.if_rx_workqueue = alloc_workqueue("ESP_IF_RX_WORK_QUEUE", WQ_HIGHPRI, 0);
	if (!adapter.if_rx_workqueue) {
		deinit_adapter();
		return NULL;
	}
	INIT_WORK(&adapter.if_rx_work, esp_if_rx_work);
	adapter.events_wq = alloc_workqueue("ESP_EVENTS_WORKQUEUE", WQ_HIGHPRI, 0);
	if (!adapter.events_wq) {
		deinit_adapter();
		return NULL;
	}
	INIT_WORK(&adapter.events_work, esp_events_work);
	INIT_WORK(&adapter.mac_flter_work, update_mac_filter);
	return &adapter;
}

static void deinit_adapter(void)
{
	cancel_delayed_work_sync(&adapter.fw_recovery_work);
	if (adapter.if_context)
		atomic_set(&adapter.state, ESP_CONTEXT_DISABLED);
	skb_queue_purge(&adapter.events_skb_q);
	if (adapter.events_wq) {
		destroy_workqueue(adapter.events_wq);
		adapter.events_wq = NULL;
	}
	if (adapter.if_rx_workqueue) {
		destroy_workqueue(adapter.if_rx_workqueue);
		adapter.if_rx_workqueue = NULL;
	}
}

static bool reset_gpio_requested = false;

static void esp_free_reset_gpio(void)
{
	if (reset_gpio_requested && resetpin != HOST_GPIO_PIN_INVALID) {
		gpio_free(resetpin);
		reset_gpio_requested = false;
	}
}

static void esp_reset(void)
{
	int ret;

	if (resetpin != HOST_GPIO_PIN_INVALID) {
		if (!gpio_is_valid(resetpin)) {
			esp_warn("host resetpin (%d) configured is invalid GPIO\n", resetpin);
			resetpin = HOST_GPIO_PIN_INVALID;
			return;
		}
		if (!reset_gpio_requested) {
			ret = gpio_request(resetpin, "esp_reset");
			if (ret) {
				esp_warn("host resetpin (%d) request failed: %d\n", resetpin, ret);
				resetpin = HOST_GPIO_PIN_INVALID;
				return;
			}
			reset_gpio_requested = true;
		}
		ret = gpio_direction_output(resetpin, true);
		if (ret) {
			esp_warn("host resetpin (%d) direction output failed: %d\n", resetpin, ret);
			esp_free_reset_gpio();
			resetpin = HOST_GPIO_PIN_INVALID;
			return;
		}
		gpio_set_value(resetpin, 0);
		udelay(200);
		gpio_direction_input(resetpin);
		esp_dbg("Triggering ESP reset.\n");
	}
}

static int __init esp_init(void)
{
	int ret = 0;
	struct esp_adapter *adapter = NULL;

	esp_reset();
	msleep(200);
	adapter = init_adapter();
	if (!adapter) {
		esp_free_reset_gpio();
		resetpin = HOST_GPIO_PIN_INVALID;
		return -EFAULT;
	}

	ret = esp_init_interface_layer(adapter, clockspeed);
	if (ret != 0) {
		deinit_adapter();
		esp_free_reset_gpio();
		resetpin = HOST_GPIO_PIN_INVALID;
		return ret;
	}

	if (debugfs_init())
		esp_warn("debugfs unavailable\n");
	return 0;
}

static void __exit esp_exit(void)
{
	uint8_t iface_idx = 0;
	struct esp_wifi_device *priv;
	bool can_deinit_fw;

	set_bit(ESP_DRIVER_UNLOADING, &adapter.state_flags);
	cancel_delayed_work_sync(&adapter.fw_recovery_work);
	if (adapter.events_wq)
		cancel_work_sync(&adapter.events_work);
	skb_queue_purge(&adapter.events_skb_q);
	cancel_work_sync(&adapter.mac_flter_work);

	can_deinit_fw = test_bit(ESP_INIT_DONE, &adapter.state_flags) &&
		atomic_read(&adapter.state) >= ESP_CONTEXT_RX_READY &&
		!test_bit(ESP_FW_RESET_EXPECTED, &adapter.state_flags) &&
		!test_bit(ESP_OTA_IN_PROGRESS, &adapter.state_flags);

	if (can_deinit_fw)
		set_bit(ESP_ALLOW_DEINIT, &adapter.state_flags);

	if (can_deinit_fw) {
		for (iface_idx = 0; iface_idx < ESP_MAX_INTERFACE; iface_idx++) {
			priv = adapter.priv[iface_idx];
			if (!priv || !test_bit(ESP_INTERFACE_INITIALIZED, &priv->priv_flags))
				continue;
			if (cmd_deinit_interface(priv))
				esp_err("Failed to deinit interface %u on ESP side\n", iface_idx);
			clear_bit(ESP_INTERFACE_INITIALIZED, &priv->priv_flags);
		}
		if (adapter.if_context)
			generate_slave_intr(adapter.if_context, BIT(ESP_CLOSE_DATA_PATH));
	} else {
		for (iface_idx = 0; iface_idx < ESP_MAX_INTERFACE; iface_idx++) {
			priv = adapter.priv[iface_idx];
			if (priv)
				clear_bit(ESP_INTERFACE_INITIALIZED, &priv->priv_flags);
		}
	}

	clear_bit(ESP_ALLOW_DEINIT, &adapter.state_flags);
	set_bit(ESP_SKIP_FW_DEINIT, &adapter.state_flags);
	set_bit(ESP_CLEANUP_IN_PROGRESS, &adapter.state_flags);
	set_bit(ESP_TRANSPORT_REMOVING, &adapter.state_flags);
	clear_bit(ESP_INIT_DONE, &adapter.state_flags);
	atomic_set(&adapter.state, ESP_CONTEXT_DISABLED);

	if (adapter.events_wq)
		cancel_work_sync(&adapter.events_work);
	skb_queue_purge(&adapter.events_skb_q);
#if TEST_RAW_TP
	if (raw_tp_mode != 0)
		test_raw_tp_cleanup();
#endif
	clear_bit(ESP_DRIVER_ACTIVE, &adapter.state_flags);
	for (iface_idx = 0; iface_idx < ESP_MAX_INTERFACE; iface_idx++) {
		priv = adapter.priv[iface_idx];
		if (priv)
			esp_mlme_cancel(priv);
	}

	esp_deinit_interface_layer();
	deinit_adapter();

	esp_free_reset_gpio();
	debugfs_exit();
}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Amey Inamdar <amey.inamdar@espressif.com>");
MODULE_AUTHOR("Mangesh Malusare <mangesh.malusare@espressif.com>");
MODULE_AUTHOR("Yogesh Mantri <yogesh.mantri@espressif.com>");
MODULE_AUTHOR("Kapil Gupta <kapil.gupta@espressif.com>");
MODULE_DESCRIPTION("Wifi driver for ESP-Hosted solution");
MODULE_VERSION(RELEASE_VERSION);
module_init(esp_init);
module_exit(esp_exit);
