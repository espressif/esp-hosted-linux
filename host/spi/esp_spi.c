// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 */
#include "utils.h"
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/module.h>
#include "esp_spi.h"
#include "esp_if.h"
#include "esp_api.h"
#include "esp_bt_api.h"
#include "esp_kernel_port.h"
#include "esp_stats.h"
#include "esp_utils.h"
#include "esp_cfg80211.h"
#include "esp_cmd.h"

#define SPI_INITIAL_CLK_MHZ     10
#define TX_MAX_PENDING_COUNT    100
#define TX_RESUME_THRESHOLD     (TX_MAX_PENDING_COUNT/5)

static uint8_t g_spi_mode = SPI_MODE_2;
static struct sk_buff *read_packet(struct esp_adapter *adapter);
static int write_packet(struct esp_adapter *adapter, struct sk_buff *skb);
static void spi_exit(void);
static int spi_init(void);
static void adjust_spi_clock(u8 spi_clk_mhz);
static void open_data_path(void);
static bool spi_accepting_work(void);
static bool spi_get_cmd_info(struct sk_buff *skb, u8 *cmd_code, u16 *cmd_seq);

static volatile u8 data_path;
volatile u8 host_sleep;
static struct esp_spi_context spi_context;
static char hardware_type = ESP_FIRMWARE_CHIP_UNRECOGNIZED;
static atomic_t tx_pending;

static struct sk_buff *esp_spi_alloc_skb(u32 len)
{
	struct sk_buff *skb = NULL;
	u32 alloc_len;
	u8 offset;

	alloc_len = max(len, (u32)SPI_BUF_SIZE) + INTERFACE_HEADER_PADDING;
	skb = netdev_alloc_skb(NULL, alloc_len);
	if (skb) {
		offset = ((unsigned long)skb->data) & (SKB_DATA_ADDR_ALIGNMENT - 1);
		if (offset)
			skb_reserve(skb, INTERFACE_HEADER_PADDING - offset);
	}
	return skb;
}

static void esp_spi_purge_queues(void)
{
	struct sk_buff *skb;
	u8 q;
	u8 cmd_code;
	u16 cmd_seq;

	for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
		while ((skb = skb_dequeue(&spi_context.tx_q[q])) != NULL) {
			if (spi_context.adapter && spi_get_cmd_info(skb, &cmd_code, &cmd_seq))
				esp_cmd_transport_failed(spi_context.adapter, cmd_code, cmd_seq, -EIO);
#if TEST_RAW_TP
			{
				struct esp_payload_header *header = (struct esp_payload_header *)skb->data;
				if (header->if_type == ESP_TEST_IF)
					esp_raw_tp_tx_failed(1);
			}
#endif
			if (atomic_read(&tx_pending) > 0)
				atomic_dec(&tx_pending);
			dev_kfree_skb(skb);
		}
		skb_queue_purge(&spi_context.rx_q[q]);
	}
	atomic_set(&tx_pending, 0);
#if TEST_RAW_TP
	if (raw_tp_mode == ESP_TEST_RAW_TP_HOST_TO_ESP)
		esp_raw_tp_queue_resume();
#endif
}

static int esp_spi_quiesce_for_fw_reset(struct esp_adapter *adapter)
{
	if (!adapter || adapter != spi_context.adapter)
		return -EINVAL;
	data_path = CLOSE_DATAPATH;
	atomic_set(&adapter->state, ESP_CONTEXT_DISABLED);
	if (spi_context.spi_workqueue)
		cancel_work_sync(&spi_context.spi_work);
	esp_spi_purge_queues();
	if (adapter->if_rx_workqueue)
		cancel_work_sync(&adapter->if_rx_work);
	return 0;
}

static int esp_spi_reinit_after_fw_reset(struct esp_adapter *adapter)
{
	if (!adapter || adapter != spi_context.adapter)
		return -EINVAL;
	open_data_path();
	atomic_set(&adapter->state, ESP_CONTEXT_READY);
	return 0;
}

static int esp_spi_recover_transport(struct esp_adapter *adapter)
{
	if (!adapter || adapter != spi_context.adapter)
		return -EINVAL;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return -ESHUTDOWN;

	atomic_set(&adapter->state, ESP_CONTEXT_RX_READY);
	if (spi_context.spi_workqueue)
		cancel_work_sync(&spi_context.spi_work);

	if (test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags)) {
		esp_spi_purge_queues();
		open_data_path();
		esp_process_new_packet_intr(adapter);
		return 0;
	}

	if (test_bit(ESP_INIT_DONE, &adapter->state_flags)) {
		atomic_set(&adapter->state, ESP_CONTEXT_READY);
		esp_process_new_packet_intr(adapter);
		return 1;
	}

	open_data_path();
	esp_process_new_packet_intr(adapter);
	return 0;
}

static void esp_spi_flush_bt_traffic(struct esp_adapter *adapter)
{
	struct sk_buff *skb, *tmp;
	struct sk_buff_head free_q;
	u8 q;
	unsigned long flags;
	bool has_work;

	__skb_queue_head_init(&free_q);
	for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
		spin_lock_irqsave(&spi_context.tx_q[q].lock, flags);
		skb_queue_walk_safe(&spi_context.tx_q[q], skb, tmp) {
			struct esp_payload_header *header;
			if (skb->len < sizeof(*header))
				continue;
			header = (struct esp_payload_header *)skb->data;
			if (header->if_type == ESP_HCI_IF) {
				__skb_unlink(skb, &spi_context.tx_q[q]);
				if (atomic_read(&tx_pending) > 0)
					atomic_dec(&tx_pending);
				__skb_queue_tail(&free_q, skb);
			}
		}
		spin_unlock_irqrestore(&spi_context.tx_q[q].lock, flags);
	}

	if (spi_context.spi_workqueue)
		cancel_work_sync(&spi_context.spi_work);

	for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
		spin_lock_irqsave(&spi_context.rx_q[q].lock, flags);
		skb_queue_walk_safe(&spi_context.rx_q[q], skb, tmp) {
			struct esp_payload_header *header;
			if (skb->len < sizeof(*header))
				continue;
			header = (struct esp_payload_header *)skb->data;
			if (header->if_type == ESP_HCI_IF) {
				__skb_unlink(skb, &spi_context.rx_q[q]);
				__skb_queue_tail(&free_q, skb);
			}
		}
		spin_unlock_irqrestore(&spi_context.rx_q[q].lock, flags);
	}

	while ((skb = __skb_dequeue(&free_q)) != NULL)
		dev_kfree_skb(skb);

	if (spi_accepting_work()) {
		has_work = gpio_get_value(SPI_DATA_READY_PIN) ||
			   !skb_queue_empty(&spi_context.tx_q[PRIO_Q_HIGH]) ||
			   !skb_queue_empty(&spi_context.tx_q[PRIO_Q_MID]) ||
			   !skb_queue_empty(&spi_context.tx_q[PRIO_Q_LOW]);
		if (has_work && spi_context.spi_workqueue)
			queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
	}
}

static struct esp_if_ops if_ops = {
	.read		= read_packet,
	.write		= write_packet,
	.alloc_skb	= esp_spi_alloc_skb,
	.quiesce_for_fw_reset = esp_spi_quiesce_for_fw_reset,
	.reinit_after_fw_reset = esp_spi_reinit_after_fw_reset,
	.recover_transport = esp_spi_recover_transport,
	.flush_bt_traffic = esp_spi_flush_bt_traffic,
};

static void open_data_path(void)
{
	atomic_set(&tx_pending, 0);
	msleep(200);
	data_path = OPEN_DATAPATH;
}

static bool spi_accepting_work(void)
{
	struct esp_adapter *adapter = spi_context.adapter;

	return data_path && adapter &&
		!test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) &&
		(!test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) ||
		 test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags));
}

static irqreturn_t spi_data_ready_interrupt_handler(int irq, void *dev)
{
	if (spi_accepting_work() && spi_context.spi_workqueue)
		queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
	return IRQ_HANDLED;
}

static irqreturn_t spi_interrupt_handler(int irq, void *dev)
{
	if (spi_accepting_work() && spi_context.spi_workqueue)
		queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
	return IRQ_HANDLED;
}

static struct sk_buff *read_packet(struct esp_adapter *adapter)
{
	struct esp_spi_context *context;
	struct sk_buff *skb = NULL;

	if (!data_path)
		return NULL;
	if (!adapter || !adapter->if_context) {
		esp_err("Invalid args\n");
		return NULL;
	}
	context = adapter->if_context;
	if (context->esp_spi_dev) {
		skb = skb_dequeue(&(context->rx_q[PRIO_Q_HIGH]));
		if (!skb)
			skb = skb_dequeue(&(context->rx_q[PRIO_Q_MID]));
		if (!skb)
			skb = skb_dequeue(&(context->rx_q[PRIO_Q_LOW]));
	} else {
		esp_err("Invalid args\n");
		return NULL;
	}
	return skb;
}

static int write_packet(struct esp_adapter *adapter, struct sk_buff *skb)
{
	u32 max_pkt_size = SPI_BUF_SIZE - sizeof(struct esp_payload_header);
	struct esp_payload_header *payload_header;
	struct esp_skb_cb *cb = NULL;
	struct sk_buff_head *tx_q;
	uint8_t prio;
	bool raw_tp = false;

	if (!adapter || !adapter->if_context || !skb || !skb->data || !skb->len) {
		esp_err("Invalid args\n");
		if (skb)
			dev_kfree_skb(skb);
		return -EINVAL;
	}

	payload_header = (struct esp_payload_header *)skb->data;
	if (skb->len > max_pkt_size) {
		esp_err("Drop pkt of len[%u] > max spi transport len[%u]\n", skb->len, max_pkt_size);
		dev_kfree_skb(skb);
		return -EPERM;
	}

	if (!data_path || atomic_read(&adapter->state) < ESP_CONTEXT_READY ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
	    (test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags))) {
		esp_info("%u datapath closed\n", __LINE__);
		dev_kfree_skb(skb);
		return -EPERM;
	}

#if TEST_RAW_TP
	raw_tp = payload_header->if_type == ESP_TEST_IF;
	if (raw_tp && atomic_read(&tx_pending) >= TX_MAX_PENDING_COUNT) {
		esp_raw_tp_queue_pause();
		if (atomic_read(&tx_pending) < TX_RESUME_THRESHOLD)
			esp_raw_tp_queue_resume();
		dev_kfree_skb(skb);
		if (spi_accepting_work() && spi_context.spi_workqueue)
			queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
		return -EBUSY;
	}
#endif

	cb = (struct esp_skb_cb *)skb->cb;
	if ((payload_header->if_type == ESP_STA_IF || payload_header->if_type == ESP_AP_IF) &&
	    cb->priv && atomic_read(&tx_pending) >= TX_MAX_PENDING_COUNT) {
		esp_tx_pause(cb->priv);
		dev_kfree_skb(skb);
		esp_verbose("TX Pause busy");
		if (spi_accepting_work() && spi_context.spi_workqueue)
			queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
		return -EBUSY;
	}

	if (payload_header->if_type == ESP_INTERNAL_IF)
		prio = PRIO_Q_HIGH;
	else if (payload_header->if_type == ESP_HCI_IF)
		prio = PRIO_Q_MID;
	else
		prio = PRIO_Q_LOW;
	tx_q = &spi_context.tx_q[prio];

	spin_lock_bh(&tx_q->lock);
	if (!data_path || atomic_read(&adapter->state) < ESP_CONTEXT_READY ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
	    (test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags))) {
		spin_unlock_bh(&tx_q->lock);
		dev_kfree_skb(skb);
		return -ESHUTDOWN;
	}
	atomic_inc(&tx_pending);
	__skb_queue_tail(tx_q, skb);
	spin_unlock_bh(&tx_q->lock);

#if TEST_RAW_TP
	if (raw_tp && atomic_read(&tx_pending) >= TX_MAX_PENDING_COUNT) {
		esp_raw_tp_queue_pause();
		if (atomic_read(&tx_pending) < TX_RESUME_THRESHOLD)
			esp_raw_tp_queue_resume();
	}
#endif

	if (spi_accepting_work() && spi_context.spi_workqueue)
		queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
	return 0;
}

int esp_validate_chipset(struct esp_adapter *adapter, u8 chipset)
{
	int ret = 0;

	switch(chipset) {
	case ESP_FIRMWARE_CHIP_ESP32:
	case ESP_FIRMWARE_CHIP_ESP32S2:
	case ESP_FIRMWARE_CHIP_ESP32S3:
	case ESP_FIRMWARE_CHIP_ESP32C2:
	case ESP_FIRMWARE_CHIP_ESP32C3:
	case ESP_FIRMWARE_CHIP_ESP32C6:
	case ESP_FIRMWARE_CHIP_ESP32C61:
	case ESP_FIRMWARE_CHIP_ESP32C5:
		adapter->chipset = chipset;
		esp_info("Chipset=%s ID=%02x detected over SPI\n", esp_chipname_from_id(chipset), chipset);
		break;
	default:
		esp_err("Unrecognized chipset ID=%02x\n", chipset);
		adapter->chipset = ESP_FIRMWARE_CHIP_UNRECOGNIZED;
		break;
	}
	return ret;
}

int esp_deinit_module(struct esp_adapter *adapter)
{
	uint8_t prio_q_idx, iface_idx;

	for (prio_q_idx = 0; prio_q_idx < MAX_PRIORITY_QUEUES; prio_q_idx++)
		skb_queue_purge(&spi_context.tx_q[prio_q_idx]);
	atomic_set(&tx_pending, 0);

	for (iface_idx = 0; iface_idx < ESP_MAX_INTERFACE; iface_idx++) {
		struct esp_wifi_device *priv = adapter->priv[iface_idx];
		esp_mark_scan_done_and_disconnect(priv, true);
	}

	esp_remove_card(adapter, false);
	for (prio_q_idx = 0; prio_q_idx < MAX_PRIORITY_QUEUES; prio_q_idx++)
		skb_queue_head_init(&spi_context.tx_q[prio_q_idx]);
	return 0;
}

static bool spi_get_cmd_info(struct sk_buff *skb, u8 *cmd_code, u16 *cmd_seq)
{
	struct esp_payload_header *payload_header;
	struct command_header *cmd;
	u16 offset;

	if (!skb || skb->len < sizeof(*payload_header))
		return false;
	payload_header = (struct esp_payload_header *)skb->data;
	if (payload_header->packet_type != PACKET_TYPE_COMMAND_REQUEST)
		return false;
	offset = esp_wire_le16_to_cpu(payload_header->offset);
	if (offset + sizeof(*cmd) > skb->len)
		return false;
	cmd = (struct command_header *)(skb->data + offset);
	if (cmd_code)
		*cmd_code = cmd->cmd_code;
	if (cmd_seq)
		*cmd_seq = esp_wire_le16_to_cpu(cmd->seq_num);
	return true;
}

static bool spi_is_hci_packet(const struct sk_buff *skb)
{
	const struct esp_payload_header *header;

	if (!skb || skb->len < sizeof(*header))
		return false;
	header = (const struct esp_payload_header *)skb->data;
	return header->if_type == ESP_HCI_IF;
}

static void spi_notify_tx_lost(bool is_cmd, bool is_hci_pkt,
		u8 cmd_code, u16 cmd_seq, int err, bool recover)
{
	struct esp_adapter *adapter = spi_context.adapter;

	if (!adapter)
		return;

	if (recover || is_hci_pkt) {
		/* A failed full-duplex transaction cannot prove whether a control
		 * side effect committed, or whether an asynchronous firmware event
		 * was consumed on the slave side. Similarly, an unsubmitted but
		 * consumed HCI packet cannot be completed to Linux without transport
		 * recovery. Quarantine the incarnation and force firmware reset
		 * recovery to prevent silent event loss or HCI timeouts. */
		esp_schedule_fw_reset_recovery(adapter);
		if (is_cmd)
			esp_cmd_transport_failed(adapter, cmd_code, cmd_seq, err);
		esp_request_firmware_restart(adapter);
		return;
	}

	if (is_cmd)
		esp_cmd_transport_failed(adapter, cmd_code, cmd_seq, err);
}

static int process_rx_buf(struct sk_buff *skb)
{
	struct esp_payload_header *header;
	u16 len = 0;
	u16 offset = 0;

	if (!skb)
		return -EINVAL;
	header = (struct esp_payload_header *)skb->data;
	if (header->if_type >= ESP_MAX_IF)
		return -EINVAL;

	offset = esp_wire_le16_to_cpu(header->offset);
	len = esp_wire_le16_to_cpu(header->len);
	if (len == 0)
		return -EINVAL;
	if (len > SPI_BUF_SIZE || !ESP_OFFSET_VALID(offset) ||
	    offset > skb->len || len > (skb->len - offset)) {
		esp_err("Drop invalid pkt: len=%d offset=%d skb_len=%u\n", len, offset, skb->len);
		return -EINVAL;
	}
	len += offset;
	skb_trim(skb, len);
	if (!data_path) {
		esp_verbose("%u datapath closed\n", __LINE__);
		return -EPERM;
	}
	if (header->if_type == ESP_INTERNAL_IF)
		skb_queue_tail(&spi_context.rx_q[PRIO_Q_HIGH], skb);
	else if (header->if_type == ESP_HCI_IF)
		skb_queue_tail(&spi_context.rx_q[PRIO_Q_MID], skb);
	else
		skb_queue_tail(&spi_context.rx_q[PRIO_Q_LOW], skb);
	esp_process_new_packet_intr(spi_context.adapter);
	return 0;
}

static void esp_spi_work(struct work_struct *work)
{
	struct spi_transfer trans;
	struct sk_buff *tx_skb = NULL, *rx_skb = NULL, *src_skb = NULL;
	struct esp_skb_cb *cb = NULL;
	struct esp_payload_header *payload_header = NULL;
	u8 *rx_buf = NULL;
	int ret = 0;
	volatile int trans_ready, rx_pending;
	bool raw_tx = false;
	u32 raw_run_id = 0;
	u32 copy_len = 0;
	u8 cmd_code = 0;
	u16 cmd_seq = 0;
	bool is_cmd = false;
	bool is_hci_pkt = false;
	bool has_tx = false;

	if (!spi_accepting_work())
		return;
	trans_ready = gpio_get_value(HANDSHAKE_PIN);
	rx_pending = gpio_get_value(SPI_DATA_READY_PIN);
	if (!trans_ready)
		return;

	if (data_path) {
		has_tx = !skb_queue_empty(&spi_context.tx_q[PRIO_Q_HIGH]) ||
			 !skb_queue_empty(&spi_context.tx_q[PRIO_Q_MID]) ||
			 !skb_queue_empty(&spi_context.tx_q[PRIO_Q_LOW]);
	}

	if (!rx_pending && !has_tx)
		return;

	rx_skb = esp_spi_alloc_skb(SPI_BUF_SIZE);
	if (!rx_skb) {
		esp_err("Failed to alloc SPI RX skb\n");
		return;
	}

	tx_skb = esp_spi_alloc_skb(SPI_BUF_SIZE);
	if (!tx_skb) {
		esp_err("Failed to alloc SPI TX skb\n");
		dev_kfree_skb(rx_skb);
		return;
	}

	if (data_path) {
		src_skb = skb_dequeue(&spi_context.tx_q[PRIO_Q_HIGH]);
		if (!src_skb)
			src_skb = skb_dequeue(&spi_context.tx_q[PRIO_Q_MID]);
		if (!src_skb)
			src_skb = skb_dequeue(&spi_context.tx_q[PRIO_Q_LOW]);
		if (src_skb) {
			if (atomic_read(&tx_pending))
				atomic_dec(&tx_pending);
			cb = (struct esp_skb_cb *)src_skb->cb;
			payload_header = (struct esp_payload_header *)src_skb->data;
			if ((payload_header->if_type == ESP_STA_IF || payload_header->if_type == ESP_AP_IF) &&
			    cb->priv && atomic_read(&tx_pending) < TX_RESUME_THRESHOLD)
				esp_tx_resume(cb->priv);
#if TEST_RAW_TP
			if (raw_tp_mode == ESP_TEST_RAW_TP_HOST_TO_ESP &&
			    atomic_read(&tx_pending) < TX_RESUME_THRESHOLD)
				esp_raw_tp_queue_resume();
#endif
		}
	}

	if (!rx_pending && !src_skb) {
		dev_kfree_skb(rx_skb);
		dev_kfree_skb(tx_skb);
		return;
	}

	memset(&trans, 0, sizeof(trans));
	trans.speed_hz = spi_context.spi_clk_mhz * NUMBER_1M;

	if (src_skb) {
		copy_len = min_t(u32, src_skb->len, (u32)SPI_BUF_SIZE);
		payload_header = (struct esp_payload_header *)src_skb->data;
		is_cmd = spi_get_cmd_info(src_skb, &cmd_code, &cmd_seq);
		is_hci_pkt = spi_is_hci_packet(src_skb);
#if TEST_RAW_TP
		if (raw_tp_mode == ESP_TEST_RAW_TP_HOST_TO_ESP &&
		    payload_header->if_type == ESP_TEST_IF) {
			u16 offset = esp_wire_le16_to_cpu(payload_header->offset);
			if (offset + sizeof(struct raw_tp_packet) <= src_skb->len) {
				struct raw_tp_packet *tp_pkt = (struct raw_tp_packet *)(src_skb->data + offset);
				raw_run_id = esp_wire_le32_to_cpu(tp_pkt->run_id);
			}
			esp_raw_tp_set_seq(payload_header, esp_raw_tp_tx_seq_get());
			if (spi_context.adapter->capabilities & ESP_CHECKSUM_ENABLED) {
				payload_header->checksum = 0;
				payload_header->checksum = esp_wire_cpu_to_le16(compute_checksum(
					src_skb->data,
					esp_wire_le16_to_cpu(payload_header->len) +
					esp_wire_le16_to_cpu(payload_header->offset)));
			}
			raw_tx = true;
		}
#endif
		skb_put_data(tx_skb, src_skb->data, copy_len);
		if (copy_len < SPI_BUF_SIZE)
			skb_put_zero(tx_skb, SPI_BUF_SIZE - copy_len);
		dev_kfree_skb(src_skb);
		src_skb = NULL;

		trans.tx_buf = tx_skb->data;
		esp_hex_dump_verbose("tx: ", trans.tx_buf, 32);
	} else {
		skb_put_zero(tx_skb, SPI_BUF_SIZE);
		trans.tx_buf = tx_skb->data;
	}

	rx_buf = skb_put(rx_skb, SPI_BUF_SIZE);
	memset(rx_buf, 0, SPI_BUF_SIZE);
	trans.rx_buf = rx_buf;
	trans.len = SPI_BUF_SIZE;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0))
	if (hardware_type == ESP_FIRMWARE_CHIP_ESP32)
		trans.cs_change = 1;
#endif

	ret = spi_sync_transfer(spi_context.esp_spi_dev, &trans, 1);
	if (ret) {
#if TEST_RAW_TP
		if (raw_tx)
			esp_raw_tp_tx_failed(1);
#endif
		esp_err("SPI Transaction failed: %d", ret);
		spi_notify_tx_lost(is_cmd, is_hci_pkt, cmd_code, cmd_seq, ret, true);
		dev_kfree_skb(rx_skb);
		dev_kfree_skb(tx_skb);
	} else {
#if TEST_RAW_TP
		if (raw_tx)
			esp_raw_tp_tx_complete(raw_run_id, 1);
#endif
		if (process_rx_buf(rx_skb))
			dev_kfree_skb(rx_skb);
		dev_kfree_skb(tx_skb);
	}
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0))
#include <linux/platform_device.h>
static int __spi_controller_match(struct device *dev, const void *data)
{
	struct spi_controller *ctlr;
	const u16 *bus_num = data;

	ctlr = container_of(dev, struct spi_controller, dev);
	if (!ctlr)
		return 0;
	return ctlr->bus_num == *bus_num;
}

static struct spi_controller *spi_busnum_to_master(u16 bus_num)
{
	struct platform_device *pdev = NULL;
	struct spi_master *master = NULL;
	struct spi_controller *ctlr = NULL;
	struct device *dev = NULL;

	pdev = platform_device_alloc("pdev", PLATFORM_DEVID_NONE);
	if (!pdev) {
		pr_err("Error: failed to allocate platform device\n");
		return NULL;
	}
	pdev->num_resources = 0;
	if (platform_device_add(pdev)) {
		pr_err("Error: failed to add platform device\n");
		platform_device_put(pdev);
		return NULL;
	}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 91))
	master = spi_alloc_host(&pdev->dev, sizeof(void *));
#else
	master = spi_alloc_master(&pdev->dev, sizeof(void *));
#endif
	if (!master) {
		pr_err("Error: failed to allocate SPI master device\n");
		platform_device_del(pdev);
		platform_device_put(pdev);
		return NULL;
	}

	dev = class_find_device(master->dev.class, NULL, &bus_num, __spi_controller_match);
	if (dev)
		ctlr = container_of(dev, struct spi_controller, dev);

	spi_master_put(master);
	platform_device_del(pdev);
	platform_device_put(pdev);
	return ctlr;
}
#endif

static int spi_dev_init(int spi_clk_mhz)
{
	int status = 0;
	struct spi_board_info esp_board = {{0}};
	struct spi_master *master = NULL;

	strscpy(esp_board.modalias, "esp_spi", sizeof(esp_board.modalias));
	esp_board.mode = g_spi_mode;
	esp_board.max_speed_hz = spi_clk_mhz * NUMBER_1M;
	esp_board.bus_num = 0;
	esp_board.chip_select = 0;

	esp_info("Using SPI MODE %d\n", g_spi_mode);
	master = spi_busnum_to_master(esp_board.bus_num);
	if (!master) {
		esp_err("Failed to obtain SPI master handle\n");
		return -ENODEV;
	}

	set_bit(ESP_SPI_BUS_CLAIMED, &spi_context.spi_flags);
	spi_context.esp_spi_dev = spi_new_device(master, &esp_board);
	/* spi_busnum_to_master() returns the class_find_device() reference.
	 * spi_new_device() takes its own controller reference, so release ours. */
	spi_master_put(master);
	master = NULL;

	if (!spi_context.esp_spi_dev) {
		esp_err("Failed to add new SPI device\n");
		return -ENODEV;
	}
	spi_context.adapter->dev = &spi_context.esp_spi_dev->dev;

	status = spi_setup(spi_context.esp_spi_dev);
	if (status) {
		esp_err("Failed to setup new SPI device");
		return status;
	}

	esp_info("Config - SPI GPIOs: Handshake[%d] Dataready[%d]\n",
		HANDSHAKE_PIN, SPI_DATA_READY_PIN);
	esp_info("Config - SPI clock[%dMHz] bus[%d] cs[%d] mode[%d]\n",
		spi_context.spi_clk_mhz, esp_board.bus_num,
		esp_board.chip_select, esp_board.mode);
	set_bit(ESP_SPI_BUS_SET, &spi_context.spi_flags);

	status = gpio_request(HANDSHAKE_PIN, "SPI_HANDSHAKE_PIN");
	if (status) {
		esp_err("Failed to obtain GPIO for Handshake pin, err:%d\n", status);
		return status;
	}
	set_bit(ESP_SPI_GPIO_HS_REQUESTED, &spi_context.spi_flags);
	status = gpio_direction_input(HANDSHAKE_PIN);
	if (status) {
		esp_err("Failed to set GPIO direction of Handshake pin, err: %d\n", status);
		return status;
	}
	status = request_irq(SPI_IRQ, spi_interrupt_handler,
			IRQF_SHARED | IRQF_TRIGGER_RISING,
			"ESP_SPI", spi_context.esp_spi_dev);
	if (status) {
		esp_err("Failed to request IRQ for Handshake pin, err:%d\n", status);
		return status;
	}
	set_bit(ESP_SPI_GPIO_HS_IRQ_DONE, &spi_context.spi_flags);

	status = gpio_request(SPI_DATA_READY_PIN, "SPI_DATA_READY_PIN");
	if (status) {
		esp_err("Failed to obtain GPIO for Data ready pin, err:%d\n", status);
		return status;
	}
	set_bit(ESP_SPI_GPIO_DR_REQUESTED, &spi_context.spi_flags);
	status = gpio_direction_input(SPI_DATA_READY_PIN);
	if (status) {
		esp_err("Failed to set GPIO direction of Data ready pin\n");
		return status;
	}
	status = request_irq(SPI_DATA_READY_IRQ, spi_data_ready_interrupt_handler,
			IRQF_SHARED | IRQF_TRIGGER_RISING,
			"ESP_SPI_DATA_READY", spi_context.esp_spi_dev);
	if (status) {
		esp_err("Failed to request IRQ for Data ready pin, err:%d\n", status);
		return status;
	}
	set_bit(ESP_SPI_GPIO_DR_IRQ_DONE, &spi_context.spi_flags);
	open_data_path();
	return 0;
}

static int spi_init(void)
{
	int status = 0;
	uint8_t prio_q_idx = 0;
	struct esp_adapter *adapter;

	spi_context.spi_workqueue = alloc_ordered_workqueue("ESP_SPI_WORK_QUEUE", 0);
	if (!spi_context.spi_workqueue) {
		esp_err("spi workqueue failed to create\n");
		spi_exit();
		return -EFAULT;
	}
	INIT_WORK(&spi_context.spi_work, esp_spi_work);
	for (prio_q_idx = 0; prio_q_idx < MAX_PRIORITY_QUEUES; prio_q_idx++) {
		skb_queue_head_init(&spi_context.tx_q[prio_q_idx]);
		skb_queue_head_init(&spi_context.rx_q[prio_q_idx]);
	}

	status = spi_dev_init(spi_context.spi_clk_mhz);
	if (status) {
		spi_exit();
		esp_err("Failed Init SPI device\n");
		return status;
	}

	adapter = spi_context.adapter;
	if (!adapter) {
		spi_exit();
		return -EFAULT;
	}
	clear_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags);
	atomic_set(&adapter->state, ESP_CONTEXT_READY);
	adapter->dev = &spi_context.esp_spi_dev->dev;
	return status;
}

static void cleanup_spi_gpio(void)
{
	if (test_bit(ESP_SPI_GPIO_HS_IRQ_DONE, &spi_context.spi_flags)) {
		free_irq(SPI_IRQ, spi_context.esp_spi_dev);
		clear_bit(ESP_SPI_GPIO_HS_IRQ_DONE, &spi_context.spi_flags);
	}
	if (test_bit(ESP_SPI_GPIO_DR_IRQ_DONE, &spi_context.spi_flags)) {
		free_irq(SPI_DATA_READY_IRQ, spi_context.esp_spi_dev);
		clear_bit(ESP_SPI_GPIO_DR_IRQ_DONE, &spi_context.spi_flags);
	}
	if (test_bit(ESP_SPI_GPIO_DR_REQUESTED, &spi_context.spi_flags)) {
		gpio_free(SPI_DATA_READY_PIN);
		clear_bit(ESP_SPI_GPIO_DR_REQUESTED, &spi_context.spi_flags);
	}
	if (test_bit(ESP_SPI_GPIO_HS_REQUESTED, &spi_context.spi_flags)) {
		gpio_free(HANDSHAKE_PIN);
		clear_bit(ESP_SPI_GPIO_HS_REQUESTED, &spi_context.spi_flags);
	}
}

static void spi_exit(void)
{
	struct esp_adapter *adapter = spi_context.adapter;

	if (adapter) {
		set_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags);
		set_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
		clear_bit(ESP_INIT_DONE, &adapter->state_flags);
		atomic_set(&adapter->state, ESP_CONTEXT_DISABLED);
		cancel_delayed_work_sync(&adapter->fw_recovery_work);
		if (adapter->events_wq)
			cancel_work_sync(&adapter->events_work);
		skb_queue_purge(&adapter->events_skb_q);
	}

	if (test_bit(ESP_SPI_GPIO_HS_IRQ_DONE, &spi_context.spi_flags))
		disable_irq(SPI_IRQ);
	if (test_bit(ESP_SPI_GPIO_DR_IRQ_DONE, &spi_context.spi_flags))
		disable_irq(SPI_DATA_READY_IRQ);

	data_path = CLOSE_DATAPATH;
	if (spi_context.spi_workqueue)
		cancel_work_sync(&spi_context.spi_work);
	esp_spi_purge_queues();
	if (adapter && adapter->if_rx_workqueue)
		cancel_work_sync(&adapter->if_rx_work);
	if (adapter)
		esp_remove_card(adapter, false);
	if (spi_context.spi_workqueue) {
		destroy_workqueue(spi_context.spi_workqueue);
		spi_context.spi_workqueue = NULL;
	}
	cleanup_spi_gpio();
	if (adapter && adapter->hcidev)
		esp_deinit_bt(adapter);
	if (adapter)
		adapter->dev = NULL;
	if (spi_context.esp_spi_dev) {
		spi_unregister_device(spi_context.esp_spi_dev);
		spi_context.esp_spi_dev = NULL;
		msleep(400);
	}
	memset(&spi_context, 0, sizeof(spi_context));
}

static void adjust_spi_clock(u8 spi_clk_mhz)
{
	if ((spi_clk_mhz) && (spi_clk_mhz != spi_context.spi_clk_mhz)) {
		esp_info("ESP Reconfigure SPI CLK to %u MHz\n", spi_clk_mhz);
		spi_context.spi_clk_mhz = spi_clk_mhz;
		spi_context.esp_spi_dev->max_speed_hz = spi_clk_mhz * NUMBER_1M;
	}
}

int esp_adjust_spi_clock(struct esp_adapter *adapter, u8 spi_clk_mhz)
{
	adjust_spi_clock(spi_clk_mhz);
	return 0;
}

int generate_slave_intr(void *context, u8 data)
{
	return 0;
}

int esp_init_interface_layer(struct esp_adapter *adapter, u32 speed)
{
	if (!adapter)
		return -EINVAL;
	/* Ambiguous SPI control transfers require a provable new firmware
	 * incarnation. Refuse to bind if the host cannot assert ESP EN/reset. */
	if (!esp_host_reset_available()) {
		esp_err("SPI requires a valid resetpin connected to ESP EN\n");
		return -EINVAL;
	}

	memset(&spi_context, 0, sizeof(spi_context));
	adapter->if_context = &spi_context;
	adapter->if_ops = &if_ops;
	adapter->if_type = ESP_IF_TYPE_SPI;
	spi_context.adapter = adapter;
	if (speed)
		spi_context.spi_clk_mhz = speed;
	else
		spi_context.spi_clk_mhz = SPI_INITIAL_CLK_MHZ;
	return spi_init();
}

void esp_deinit_interface_layer(void)
{
	spi_exit();
}
