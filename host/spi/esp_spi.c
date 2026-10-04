// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 */
#include "utils.h"
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/property.h>
#include "esp_spi.h"
#include "esp_if.h"
#include "esp_api.h"
#include "esp_bt_api.h"
#include "esp_rcp_api.h"
#include "esp_kernel_port.h"
#include "esp_stats.h"
#include "esp_utils.h"
#include "esp_cfg80211.h"
#include "esp_cmd.h"

#define SPI_INITIAL_CLK_MHZ     10
#define SPI_MAX_CLK_MHZ         40U
#define TX_MAX_PENDING_COUNT    100
#define TX_RESUME_THRESHOLD     (TX_MAX_PENDING_COUNT/5)

static bool spi_cs_change;
module_param(spi_cs_change, bool, 0644);
MODULE_PARM_DESC(spi_cs_change,
		 "Set cs_change on ESP SPI single-transfer messages for controller-specific CS hold/toggle behavior");

static struct sk_buff *read_packet(struct esp_adapter *adapter);
static int write_packet(struct esp_adapter *adapter, struct sk_buff *skb);
static void spi_exit(void);
static int spi_init(void);
static void adjust_spi_clock(u8 spi_clk_mhz);
static void open_data_path(void);
static bool spi_accepting_work(void);
static void esp_spi_pause_for_transport_recovery(struct esp_adapter *adapter);
static bool spi_get_cmd_info(struct sk_buff *skb, u8 *cmd_code, u16 *cmd_seq);
static int esp_spi_reset_target(struct esp_adapter *adapter);

static volatile u8 data_path;
volatile u8 host_sleep;
static struct esp_spi_context spi_context;
static atomic_t tx_pending;
static DEFINE_MUTEX(spi_reset_lock);
static bool spi_driver_registered;

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

static int esp_spi_gpio_asserted(struct gpio_desc *gpio, const char *name,
		bool *asserted)
{
	int value;

	if (!gpio || !asserted)
		return -EINVAL;

	value = gpiod_get_value_cansleep(gpio);
	if (value < 0) {
		esp_err("Failed to read SPI %s GPIO: %d\n", name, value);
		return value;
	}

	*asserted = value != 0;
	return 0;
}

static unsigned long esp_spi_gpio_irq_flags(struct gpio_desc *gpio)
{
	return gpiod_is_active_low(gpio) ?
		IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;
}

static void esp_spi_kick(void)
{
	if (spi_accepting_work() && spi_context.spi_workqueue)
		queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
}

static void esp_spi_requeue_if_pending(void)
{
	bool handshake = false;
	bool data_ready = false;
	bool has_tx = false;

	if (!spi_accepting_work() || !spi_context.spi_workqueue)
		return;

	if (esp_spi_gpio_asserted(spi_context.handshake_gpio,
				 "handshake", &handshake) ||
	    esp_spi_gpio_asserted(spi_context.data_ready_gpio,
				 "data-ready", &data_ready)) {
		esp_spi_pause_for_transport_recovery(spi_context.adapter);
		return;
	}

	/*
	 * Do not spin while the slave is not ready; the next handshake edge will
	 * schedule us. When READY remains asserted, however, queue_work() calls
	 * from multiple producers may have coalesced while this worker was
	 * pending/running, so explicitly drain the remaining level-triggered
	 * work one transfer at a time.
	 */
	if (!handshake)
		return;

	if (atomic_read(&spi_context.adapter->state) >= ESP_CONTEXT_READY)
		has_tx = !skb_queue_empty(&spi_context.tx_q[PRIO_Q_HIGH]) ||
			 !skb_queue_empty(&spi_context.tx_q[PRIO_Q_MID]) ||
			 !skb_queue_empty(&spi_context.tx_q[PRIO_Q_LOW]);

	if (data_ready || has_tx)
		queue_work(spi_context.spi_workqueue, &spi_context.spi_work);
}

static void esp_spi_pause_for_transport_recovery(struct esp_adapter *adapter)
{
	/*
	 * Stop IRQ/work re-entry immediately. Existing TX queues remain intact
	 * and are resumed if recover_transport() proves the same firmware session
	 * is still usable.
	 */
	data_path = CLOSE_DATAPATH;
	esp_schedule_transport_recovery(adapter);
}

static int esp_spi_quiesce_for_fw_reset(struct esp_adapter *adapter)
{
	if (!adapter || adapter != spi_context.adapter)
		return -EINVAL;
	/*
	 * A boot event already queued before a requested physical reset belongs
	 * to the old incarnation. Reject it until ordered reset_work completes;
	 * the recovery path will consume the new boot event afterwards.
	 */
	if (test_bit(ESP_SPI_RESETTING, &spi_context.spi_flags))
		return -EAGAIN;
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
	int ret;

	if (!adapter || adapter != spi_context.adapter)
		return -EINVAL;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return -ESHUTDOWN;
	if (test_bit(ESP_SPI_RESETTING, &spi_context.spi_flags))
		return -EAGAIN;
	if (test_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags)) {
		ret = esp_spi_reset_target(adapter);
		if (ret)
			return ret;
		return -EAGAIN;
	}

	atomic_set(&adapter->state, ESP_CONTEXT_RX_READY);
	if (spi_context.spi_workqueue)
		cancel_work_sync(&spi_context.spi_work);

	if (test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags)) {
		esp_spi_purge_queues();
		open_data_path();
		esp_spi_kick();
		esp_process_new_packet_intr(adapter);
		return 0;
	}

	if (test_bit(ESP_INIT_DONE, &adapter->state_flags)) {
		data_path = OPEN_DATAPATH;
		atomic_set(&adapter->state, ESP_CONTEXT_READY);
		esp_spi_kick();
		esp_process_new_packet_intr(adapter);
		return 1;
	}

	open_data_path();
	esp_spi_kick();
	esp_process_new_packet_intr(adapter);
	return 0;
}

static void esp_spi_flush_if_traffic(struct esp_adapter *adapter, u8 if_type)
{
	struct sk_buff *skb, *tmp;
	struct sk_buff_head free_q;
	u8 q;
	unsigned long flags;
	bool has_work;
	bool data_ready = false;

	__skb_queue_head_init(&free_q);
	for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
		spin_lock_irqsave(&spi_context.tx_q[q].lock, flags);
		skb_queue_walk_safe(&spi_context.tx_q[q], skb, tmp) {
			struct esp_payload_header *header;
			if (skb->len < sizeof(*header))
				continue;
			header = (struct esp_payload_header *)skb->data;
			if (header->if_type == if_type) {
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
			if (header->if_type == if_type) {
				__skb_unlink(skb, &spi_context.rx_q[q]);
				__skb_queue_tail(&free_q, skb);
			}
		}
		spin_unlock_irqrestore(&spi_context.rx_q[q].lock, flags);
	}

	while ((skb = __skb_dequeue(&free_q)) != NULL)
		dev_kfree_skb(skb);

	if (spi_accepting_work()) {
		if (esp_spi_gpio_asserted(spi_context.data_ready_gpio,
					 "data-ready", &data_ready)) {
			esp_spi_pause_for_transport_recovery(adapter);
			return;
		}
		has_work = data_ready ||
			   !skb_queue_empty(&spi_context.tx_q[PRIO_Q_HIGH]) ||
			   !skb_queue_empty(&spi_context.tx_q[PRIO_Q_MID]) ||
			   !skb_queue_empty(&spi_context.tx_q[PRIO_Q_LOW]);
		if (has_work)
			esp_spi_kick();
	}
}

static void esp_spi_flush_bt_traffic(struct esp_adapter *adapter)
{
	esp_spi_flush_if_traffic(adapter, ESP_HCI_IF);
}

static void esp_spi_flush_rcp_traffic(struct esp_adapter *adapter)
{
	esp_spi_flush_if_traffic(adapter, ESP_RCP_IF);
}

static void esp_spi_restore_startup_clock(void)
{
	u32 max_mhz;

	spi_context.spi_clk_mhz = spi_context.requested_clk_mhz;
	max_mhz = spi_context.spi_max_hz / NUMBER_1M;
	if (max_mhz && spi_context.spi_clk_mhz > max_mhz)
		spi_context.spi_clk_mhz = min_t(u32, max_mhz, 255U);
	if (spi_context.esp_spi_dev)
		spi_context.esp_spi_dev->max_speed_hz =
			(u32)spi_context.spi_clk_mhz * NUMBER_1M;
}

static int esp_spi_hw_reset(struct esp_adapter *adapter)
{
	int ret = 0;

	if (!adapter || adapter != spi_context.adapter)
		return -EINVAL;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return -ESHUTDOWN;

	mutex_lock(&spi_reset_lock);
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (!spi_context.reset_gpio) {
		ret = -ENODEV;
		goto out;
	}

	ret = gpiod_direction_output(spi_context.reset_gpio, 0);
	if (ret) {
		esp_err("Failed to configure SPI reset GPIO as output: %d\n", ret);
		goto out;
	}
	spi_context.reset_gpio_driven = true;

	esp_info("Resetting ESP SPI target via reset-gpios\n");
	gpiod_set_value_cansleep(spi_context.reset_gpio, 1);
	usleep_range(200, 500);
	gpiod_set_value_cansleep(spi_context.reset_gpio, 0);
	msleep(500);
	/*
	 * Firmware restart returns the peripheral to its startup SPI timing.
	 * Do not try to receive the new boot event at a clock negotiated by the
	 * previous incarnation.
	 */
	esp_spi_restore_startup_clock();
out:
	mutex_unlock(&spi_reset_lock);
	return ret;
}

static void esp_spi_reset_work(struct work_struct *work)
{
	struct esp_spi_context *context =
		container_of(work, struct esp_spi_context, reset_work);
	struct esp_adapter *adapter = context->adapter;
	int ret;

	ret = esp_spi_hw_reset(adapter);
	if (ret) {
		if (ret != -ESHUTDOWN && adapter) {
			set_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);
			esp_err("SPI target reset failed: %d\n", ret);
		}
	} else if (adapter) {
		clear_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);
	}

	clear_bit(ESP_SPI_RESETTING, &context->spi_flags);

	/*
	 * The reset work is ordered behind all SPI transactions. Restart the
	 * recovery quiet window only after EN has been released, so recovery can
	 * never reopen the data path while the physical reset is still active.
	 */
	if (adapter &&
	    !test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) &&
	    !test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		mod_delayed_work(system_wq, &adapter->fw_recovery_work,
				 msecs_to_jiffies(ESP_FW_RECOVERY_QUIET_MS));
}

static int esp_spi_reset_target(struct esp_adapter *adapter)
{
	if (!adapter || adapter != spi_context.adapter)
		return -EINVAL;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return -ESHUTDOWN;
	if (!spi_context.reset_gpio || !spi_context.spi_workqueue)
		return -ENODEV;

	/*
	 * Gate new transfers before queueing reset. reset_work shares the ordered
	 * SPI workqueue with spi_work, so any transfer already in flight finishes
	 * first and no later transfer can start until reset has completed.
	 */
	if (test_and_set_bit(ESP_SPI_RESETTING, &spi_context.spi_flags))
		return 0;

	data_path = CLOSE_DATAPATH;
	if (atomic_read(&adapter->state) > ESP_CONTEXT_RX_READY)
		atomic_set(&adapter->state, ESP_CONTEXT_RX_READY);

	if (!queue_work(spi_context.spi_workqueue, &spi_context.reset_work)) {
		clear_bit(ESP_SPI_RESETTING, &spi_context.spi_flags);
		return -EBUSY;
	}
	return 0;
}

static struct esp_if_ops if_ops = {
	.read		= read_packet,
	.write		= write_packet,
	.alloc_skb	= esp_spi_alloc_skb,
	.quiesce_for_fw_reset = esp_spi_quiesce_for_fw_reset,
	.reinit_after_fw_reset = esp_spi_reinit_after_fw_reset,
	.reset_target = esp_spi_reset_target,
	.recover_transport = esp_spi_recover_transport,
	.flush_bt_traffic = esp_spi_flush_bt_traffic,
	.flush_rcp_traffic = esp_spi_flush_rcp_traffic,
};

static void open_data_path(void)
{
	atomic_set(&tx_pending, 0);
	msleep(200);
	data_path = OPEN_DATAPATH;
	esp_spi_kick();
}

static bool spi_accepting_work(void)
{
	struct esp_adapter *adapter = spi_context.adapter;

	return data_path && adapter &&
		!test_bit(ESP_SPI_RESETTING, &spi_context.spi_flags) &&
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
	else if (payload_header->if_type == ESP_HCI_IF ||
		 payload_header->if_type == ESP_RCP_IF)
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
	int ret = -ENODEV;

	switch(chipset) {
	case ESP_FIRMWARE_CHIP_ESP32:
	case ESP_FIRMWARE_CHIP_ESP32S2:
	case ESP_FIRMWARE_CHIP_ESP32S3:
	case ESP_FIRMWARE_CHIP_ESP32C2:
	case ESP_FIRMWARE_CHIP_ESP32C3:
	case ESP_FIRMWARE_CHIP_ESP32C6:
	case ESP_FIRMWARE_CHIP_ESP32C61:
	case ESP_FIRMWARE_CHIP_ESP32C5:
	case ESP_FIRMWARE_CHIP_ESP32S31:
		adapter->chipset = chipset;
		ret = 0;
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

static bool spi_is_stateful_packet(const struct sk_buff *skb)
{
	const struct esp_payload_header *header;

	if (!skb || skb->len < sizeof(*header))
		return false;
	header = (const struct esp_payload_header *)skb->data;
	return header->if_type == ESP_HCI_IF || header->if_type == ESP_RCP_IF;
}

static void spi_notify_tx_lost(bool is_cmd, bool is_stateful_pkt,
		u8 cmd_code, u16 cmd_seq, int err, bool recover)
{
	struct esp_adapter *adapter = spi_context.adapter;

	if (!adapter)
		return;

	if (recover || is_stateful_pkt) {
		/* A failed full-duplex transaction cannot prove whether a control
		 * side effect committed, or whether an asynchronous firmware event
		 * was consumed on the slave side. HCI and RCP are stateful streams:
		 * silent loss would desynchronize HCI or Spinel. Quarantine the
		 * incarnation and force firmware reset recovery. */
		esp_schedule_fw_reset_recovery(adapter);
		if (is_cmd)
			esp_cmd_transport_failed(adapter, cmd_code, cmd_seq, err);
		esp_request_firmware_restart(adapter);
		return;
	}

	if (is_cmd)
		esp_cmd_transport_failed(adapter, cmd_code, cmd_seq, err);
}

static void esp_spi_fail_rcp_rx(const char *reason)
{
	struct esp_adapter *adapter = spi_context.adapter;

	if (!adapter || !esp_rcp_fail_closed_required(adapter))
		return;

	esp_err("RCP SPI RX stream may be truncated: %s; resetting session\n",
		reason);
	esp_schedule_fw_reset_recovery(adapter);
	esp_request_firmware_restart(adapter);
}

static int process_rx_buf(struct sk_buff *skb)
{
	struct esp_payload_header *header;
	u16 len = 0;
	u16 offset = 0;

	if (!skb)
		return -EINVAL;
	header = (struct esp_payload_header *)skb->data;
	if (header->if_type >= ESP_MAX_IF) {
		esp_spi_fail_rcp_rx("invalid interface type");
		return -EINVAL;
	}

	offset = esp_wire_le16_to_cpu(header->offset);
	len = esp_wire_le16_to_cpu(header->len);
	if (len == 0) {
		esp_spi_fail_rcp_rx("zero payload");
		return -EINVAL;
	}
	if (len > SPI_BUF_SIZE || !ESP_OFFSET_VALID(offset) ||
	    offset > skb->len || len > (skb->len - offset)) {
		esp_err("Drop invalid pkt: len=%d offset=%d skb_len=%u\n", len, offset, skb->len);
		esp_spi_fail_rcp_rx("invalid length/offset");
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
	else if (header->if_type == ESP_HCI_IF || header->if_type == ESP_RCP_IF)
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
	bool trans_ready, rx_pending;
	bool raw_tx = false;
	u32 raw_run_id = 0;
	u32 copy_len = 0;
	u8 cmd_code = 0;
	u16 cmd_seq = 0;
	bool is_cmd = false;
	bool is_stateful_pkt = false;
	bool has_tx = false;

	if (!spi_accepting_work())
		return;
	ret = esp_spi_gpio_asserted(spi_context.handshake_gpio,
				    "handshake", &trans_ready);
	if (ret) {
		esp_spi_pause_for_transport_recovery(spi_context.adapter);
		return;
	}
	ret = esp_spi_gpio_asserted(spi_context.data_ready_gpio,
				    "data-ready", &rx_pending);
	if (ret) {
		esp_spi_pause_for_transport_recovery(spi_context.adapter);
		return;
	}
	if (!trans_ready)
		return;

	if (data_path &&
	    atomic_read(&spi_context.adapter->state) >= ESP_CONTEXT_READY) {
		has_tx = !skb_queue_empty(&spi_context.tx_q[PRIO_Q_HIGH]) ||
			 !skb_queue_empty(&spi_context.tx_q[PRIO_Q_MID]) ||
			 !skb_queue_empty(&spi_context.tx_q[PRIO_Q_LOW]);
	}

	if (!rx_pending && !has_tx)
		return;

	rx_skb = esp_spi_alloc_skb(SPI_BUF_SIZE);
	if (!rx_skb) {
		esp_err("Failed to alloc SPI RX skb; pausing for transport recovery\n");
		esp_spi_pause_for_transport_recovery(spi_context.adapter);
		return;
	}

	tx_skb = esp_spi_alloc_skb(SPI_BUF_SIZE);
	if (!tx_skb) {
		esp_err("Failed to alloc SPI TX skb; pausing for transport recovery\n");
		dev_kfree_skb(rx_skb);
		esp_spi_pause_for_transport_recovery(spi_context.adapter);
		return;
	}

	if (data_path &&
	    atomic_read(&spi_context.adapter->state) >= ESP_CONTEXT_READY) {
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
		is_stateful_pkt = spi_is_stateful_packet(src_skb);
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
	if (spi_cs_change)
		trans.cs_change = 1;
#endif

	/*
	 * A reset/recovery request may arrive after this worker dequeues TX but
	 * before the controller transfer starts. Never send another packet once
	 * that gate has closed.
	 */
	if (!spi_accepting_work()) {
#if TEST_RAW_TP
		if (raw_tx)
			esp_raw_tp_tx_failed(1);
#endif
		/*
		 * The skb has already been consumed from the host queue. Commands
		 * can be failed explicitly; HCI ownership is commit-ambiguous and
		 * must quarantine the firmware incarnation rather than disappear
		 * across the reset boundary.
		 */
		spi_notify_tx_lost(is_cmd, is_stateful_pkt, cmd_code, cmd_seq,
				   -ESHUTDOWN, false);
		dev_kfree_skb(rx_skb);
		dev_kfree_skb(tx_skb);
		return;
	}

	ret = spi_sync_transfer(spi_context.esp_spi_dev, &trans, 1);
	if (ret) {
#if TEST_RAW_TP
		if (raw_tx)
			esp_raw_tp_tx_failed(1);
#endif
		esp_err("SPI Transaction failed: %d", ret);
		spi_notify_tx_lost(is_cmd, is_stateful_pkt, cmd_code, cmd_seq, ret, true);
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
		esp_spi_requeue_if_pending();
	}
}

static void spi_release_reset_gpio(void *data)
{
	struct esp_spi_context *context = data;

	if (!context)
		return;

	mutex_lock(&spi_reset_lock);
	if (context->reset_gpio) {
		/*
		 * Leave EN/reset electrically released after unbind only when this
		 * driver actually changed the line to output. A deferred/failed
		 * probe must not perturb firmware or bootloader GPIO direction.
		 */
		if (context->reset_gpio_driven &&
		    gpiod_direction_input(context->reset_gpio))
			esp_warn("Failed to release SPI reset GPIO as input\n");
		context->reset_gpio_driven = false;
		context->reset_gpio = NULL;
	}
	mutex_unlock(&spi_reset_lock);
}

static int esp_spi_probe(struct spi_device *spi)
{
	u32 requested_hz;
	u32 dt_max_hz = 0;
	u32 max_mhz;
	u32 original_speed_hz;
	typeof(spi->mode) original_mode;
	u8 original_bits_per_word;
	bool spi_setup_applied = false;
	int restore_status;
	int status;

	if (!spi || !spi_context.adapter)
		return -ENODEV;
	if (spi_context.esp_spi_dev)
		return -EBUSY;

	spi_context.esp_spi_dev = spi;
	spi_set_drvdata(spi, &spi_context);
	original_speed_hz = spi->max_speed_hz;
	original_mode = spi->mode;
	original_bits_per_word = spi->bits_per_word;

	/*
	 * A previous unbind leaves the persistent adapter quarantined. Keep that
	 * state until all DT/controller/GPIO/IRQ resources have been validated.
	 * A failed or deferred probe must never publish a half-bound transport.
	 *
	 * spi->max_speed_hz is mutable driver state: we lower it to the current
	 * transfer speed below. Never reuse that field as the firmware/DT safety
	 * cap on a later deferred-probe retry. Re-read the immutable property on
	 * every probe instead.
	 */
	status = device_property_read_u32(&spi->dev, "spi-max-frequency",
					  &dt_max_hz);
	if (status || !dt_max_hz) {
		esp_err("Device Tree must provide spi-max-frequency\n");
		status = status ? status : -EINVAL;
		goto err_clear_dev;
	}

	if (dt_max_hz > (u32)SPI_MAX_CLK_MHZ * NUMBER_1M)
		esp_warn("Device Tree SPI cap %u Hz exceeds protocol maximum %u MHz; limiting it\n",
			 dt_max_hz, SPI_MAX_CLK_MHZ);
	spi_context.spi_max_hz = min_t(u32, dt_max_hz,
				       (u32)SPI_MAX_CLK_MHZ * NUMBER_1M);
	max_mhz = spi_context.spi_max_hz / NUMBER_1M;
	if (!max_mhz) {
		esp_err("spi-max-frequency is below 1 MHz\n");
		status = -EINVAL;
		goto err_clear_dev;
	}

	requested_hz = (u32)spi_context.requested_clk_mhz * NUMBER_1M;
	if (requested_hz > spi_context.spi_max_hz)
		esp_warn("SPI clock %u MHz exceeds Device Tree cap %u Hz; capping to %u MHz\n",
			 spi_context.requested_clk_mhz, spi_context.spi_max_hz, max_mhz);
	esp_spi_restore_startup_clock();

	if (device_property_read_bool(&spi->dev, "spi-cs-high")) {
		/*
		 * The ESP wire protocol and shipped boards use active-low CS.
		 * Reject an explicit firmware request for active-high CS. Do not
		 * infer physical polarity from SPI_CS_HIGH itself: the SPI core may
		 * set that bit internally for GPIO-backed chip selects while
		 * gpiolib performs the electrical inversion.
		 */
		esp_err("Device Tree property spi-cs-high is not supported\n");
		status = -EINVAL;
		goto err_clear_dev;
	}

	if (spi->mode & ~(SPI_CPHA | SPI_CPOL | SPI_CS_HIGH)) {
		/*
		 * CPOL/CPHA plus core-managed SPI_CS_HIGH are the only mode bits
		 * accepted by this full-duplex single-lane protocol. This rejects
		 * 3-wire, LSB-first, loopback, dual/quad/octal and NO_TX/NO_RX
		 * semantics without maintaining a version-specific deny-list.
		 */
		esp_err("Unsupported SPI mode flags 0x%x\n", spi->mode);
		status = -EINVAL;
		goto err_clear_dev;
	}

	/*
	 * Keep SPI_CPOL/SPI_CPHA exactly as parsed by the SPI core from Device
	 * Tree. Board timing belongs in firmware description, not static C data.
	 */
	spi->max_speed_hz = (u32)spi_context.spi_clk_mhz * NUMBER_1M;
	spi->bits_per_word = 8;

	status = spi_setup(spi);
	if (status) {
		esp_err("Failed to setup SPI device: %d\n", status);
		goto err_clear_dev;
	}
	spi_setup_applied = true;
	if (spi->mode & ~(SPI_CPHA | SPI_CPOL | SPI_CS_HIGH)) {
		esp_err("SPI controller restored unsupported mode flags 0x%x after setup\n",
			spi->mode);
		status = -EINVAL;
		goto err_clear_dev;
	}

	esp_info("Config - SPI clock[%uMHz] mode[%u] max[%uHz]\n",
		 spi_context.spi_clk_mhz, spi->mode, spi_context.spi_max_hz);
	set_bit(ESP_SPI_BUS_CLAIMED, &spi_context.spi_flags);
	set_bit(ESP_SPI_BUS_SET, &spi_context.spi_flags);

	spi_context.reset_gpio =
		devm_gpiod_get(&spi->dev, "reset", GPIOD_IN);
	if (IS_ERR(spi_context.reset_gpio)) {
		status = PTR_ERR(spi_context.reset_gpio);
		spi_context.reset_gpio = NULL;
		esp_err("Failed to get reset GPIO: %d\n", status);
		goto err_clear_dev;
	}
	spi_context.handshake_gpio =
		devm_gpiod_get(&spi->dev, "handshake", GPIOD_IN);
	if (IS_ERR(spi_context.handshake_gpio)) {
		status = PTR_ERR(spi_context.handshake_gpio);
		spi_context.handshake_gpio = NULL;
		esp_err("Failed to get handshake GPIO: %d\n", status);
		goto err_clear_dev;
	}
	set_bit(ESP_SPI_GPIO_HS_REQUESTED, &spi_context.spi_flags);

	spi_context.data_ready_gpio =
		devm_gpiod_get(&spi->dev, "data-ready", GPIOD_IN);
	if (IS_ERR(spi_context.data_ready_gpio)) {
		status = PTR_ERR(spi_context.data_ready_gpio);
		spi_context.data_ready_gpio = NULL;
		esp_err("Failed to get data-ready GPIO: %d\n", status);
		goto err_clear_dev;
	}
	set_bit(ESP_SPI_GPIO_DR_REQUESTED, &spi_context.spi_flags);

	spi_context.handshake_irq = gpiod_to_irq(spi_context.handshake_gpio);
	if (spi_context.handshake_irq < 0) {
		status = spi_context.handshake_irq;
		esp_err("Failed to map handshake GPIO to IRQ: %d\n", status);
		goto err_clear_dev;
	}
	status = devm_request_irq(&spi->dev, spi_context.handshake_irq,
				  spi_interrupt_handler,
				  esp_spi_gpio_irq_flags(spi_context.handshake_gpio),
				  "ESP_SPI_HANDSHAKE", spi);
	if (status) {
		esp_err("Failed to request handshake IRQ: %d\n", status);
		goto err_clear_dev;
	}
	set_bit(ESP_SPI_GPIO_HS_IRQ_DONE, &spi_context.spi_flags);

	spi_context.data_ready_irq = gpiod_to_irq(spi_context.data_ready_gpio);
	if (spi_context.data_ready_irq < 0) {
		status = spi_context.data_ready_irq;
		esp_err("Failed to map data-ready GPIO to IRQ: %d\n", status);
		goto err_clear_dev;
	}
	status = devm_request_irq(&spi->dev, spi_context.data_ready_irq,
				  spi_data_ready_interrupt_handler,
				  esp_spi_gpio_irq_flags(spi_context.data_ready_gpio),
				  "ESP_SPI_DATA_READY", spi);
	if (status) {
		esp_err("Failed to request data-ready IRQ: %d\n", status);
		goto err_clear_dev;
	}
	set_bit(ESP_SPI_GPIO_DR_IRQ_DONE, &spi_context.spi_flags);

	/*
	 * All providers and IRQ resources are now proven. Temporarily release the
	 * persistent removal quarantine only for the transport-owned reset. If
	 * reset fails, err_clear_dev publishes removal again before returning.
	 */
	clear_bit(ESP_TRANSPORT_REMOVING, &spi_context.adapter->state_flags);
	clear_bit(ESP_CLEANUP_IN_PROGRESS, &spi_context.adapter->state_flags);
	status = esp_spi_hw_reset(spi_context.adapter);
	if (status)
		goto err_clear_dev;

	/* Fresh physical reset starts a new transport incarnation. Publish the
	 * binding only now, and stay RX_READY until the boot TLV reconstructs
	 * and commits the card. */
	clear_bit(ESP_INIT_DONE, &spi_context.adapter->state_flags);
	clear_bit(ESP_FW_RECOVERY_PENDING, &spi_context.adapter->state_flags);
	clear_bit(ESP_FW_RESET_EXPECTED, &spi_context.adapter->state_flags);
	clear_bit(ESP_FW_RESTART_NEEDED, &spi_context.adapter->state_flags);
	spi_context.adapter->dev = &spi->dev;
	atomic_set(&spi_context.adapter->state, ESP_CONTEXT_RX_READY);
	open_data_path();
	esp_spi_kick();
	schedule_delayed_work(&spi_context.adapter->fw_recovery_work,
			      msecs_to_jiffies(ESP_FW_RECOVERY_WATCHDOG_MS));
	return 0;

err_clear_dev:
	data_path = CLOSE_DATAPATH;
	if (spi_context.adapter) {
		set_bit(ESP_TRANSPORT_REMOVING, &spi_context.adapter->state_flags);
		set_bit(ESP_CLEANUP_IN_PROGRESS, &spi_context.adapter->state_flags);
		clear_bit(ESP_INIT_DONE, &spi_context.adapter->state_flags);
		atomic_set(&spi_context.adapter->state, ESP_CONTEXT_DISABLED);
	}
	if (test_bit(ESP_SPI_GPIO_HS_IRQ_DONE, &spi_context.spi_flags)) {
		devm_free_irq(&spi->dev, spi_context.handshake_irq, spi);
		clear_bit(ESP_SPI_GPIO_HS_IRQ_DONE, &spi_context.spi_flags);
	}
	if (test_bit(ESP_SPI_GPIO_DR_IRQ_DONE, &spi_context.spi_flags)) {
		devm_free_irq(&spi->dev, spi_context.data_ready_irq, spi);
		clear_bit(ESP_SPI_GPIO_DR_IRQ_DONE, &spi_context.spi_flags);
	}
	if (spi_context.spi_workqueue)
		cancel_work_sync(&spi_context.spi_work);
	spi_release_reset_gpio(&spi_context);
	clear_bit(ESP_SPI_GPIO_HS_REQUESTED, &spi_context.spi_flags);
	clear_bit(ESP_SPI_GPIO_DR_REQUESTED, &spi_context.spi_flags);
	clear_bit(ESP_SPI_BUS_SET, &spi_context.spi_flags);
	clear_bit(ESP_SPI_BUS_CLAIMED, &spi_context.spi_flags);
	spi_context.handshake_gpio = NULL;
	spi_context.data_ready_gpio = NULL;
	spi_context.handshake_irq = 0;
	spi_context.data_ready_irq = 0;
	spi_context.spi_max_hz = 0;
	/*
	 * Leave an unbound/deferred device exactly as the core presented it.
	 * If the controller accepted our temporary setup, restore both the
	 * spi_device fields and controller-side configuration. Preserve the
	 * original probe error even if this best-effort restoration fails.
	 * The next probe will also re-read spi-max-frequency from firmware.
	 */
	spi->max_speed_hz = original_speed_hz;
	spi->mode = original_mode;
	spi->bits_per_word = original_bits_per_word;
	if (spi_setup_applied) {
		restore_status = spi_setup(spi);
		if (restore_status)
			esp_warn("Failed to restore SPI device configuration after probe failure: %d\n",
				 restore_status);

		/*
		 * spi_setup() may normalize fields such as bits_per_word while
		 * programming the controller. Keep the unbound device's software
		 * snapshot exactly as it was before this probe.
		 */
		spi->max_speed_hz = original_speed_hz;
		spi->mode = original_mode;
		spi->bits_per_word = original_bits_per_word;
	}
	spi_set_drvdata(spi, NULL);
	if (spi_context.adapter)
		spi_context.adapter->dev = NULL;
	spi_context.esp_spi_dev = NULL;
	return status;
}

static void esp_spi_remove_common(struct spi_device *spi)
{
	struct esp_spi_context *context = spi_get_drvdata(spi);
	struct esp_adapter *adapter;

	if (!context)
		return;
	adapter = context->adapter;

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

	data_path = CLOSE_DATAPATH;
	if (context->spi_workqueue)
		cancel_work_sync(&context->reset_work);
	clear_bit(ESP_SPI_RESETTING, &context->spi_flags);
	if (test_bit(ESP_SPI_GPIO_HS_IRQ_DONE, &context->spi_flags)) {
		devm_free_irq(&spi->dev, context->handshake_irq, spi);
		clear_bit(ESP_SPI_GPIO_HS_IRQ_DONE, &context->spi_flags);
	}
	if (test_bit(ESP_SPI_GPIO_DR_IRQ_DONE, &context->spi_flags)) {
		devm_free_irq(&spi->dev, context->data_ready_irq, spi);
		clear_bit(ESP_SPI_GPIO_DR_IRQ_DONE, &context->spi_flags);
	}
	if (context->spi_workqueue)
		cancel_work_sync(&context->spi_work);
	esp_spi_purge_queues();
	if (adapter && adapter->if_rx_workqueue)
		cancel_work_sync(&adapter->if_rx_work);

#if TEST_RAW_TP
	if (raw_tp_mode != 0 &&
	    (!adapter || !test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags)))
		test_raw_tp_cleanup();
#endif
	if (adapter) {
		/* esp_remove_card() already tears down Bluetooth when present. */
		esp_remove_card(adapter, false);
		adapter->dev = NULL;
	}

	spi_release_reset_gpio(context);
	clear_bit(ESP_SPI_GPIO_HS_REQUESTED, &context->spi_flags);
	clear_bit(ESP_SPI_GPIO_DR_REQUESTED, &context->spi_flags);
	clear_bit(ESP_SPI_BUS_SET, &context->spi_flags);
	clear_bit(ESP_SPI_BUS_CLAIMED, &context->spi_flags);
	context->handshake_gpio = NULL;
	context->data_ready_gpio = NULL;
	context->handshake_irq = 0;
	context->data_ready_irq = 0;
	context->spi_max_hz = 0;
	context->spi_clk_mhz = context->requested_clk_mhz;
	context->esp_spi_dev = NULL;
	spi_set_drvdata(spi, NULL);
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0))
static int esp_spi_remove(struct spi_device *spi)
{
	esp_spi_remove_common(spi);
	return 0;
}
#else
static void esp_spi_remove(struct spi_device *spi)
{
	esp_spi_remove_common(spi);
}
#endif

static const struct of_device_id esp_spi_of_match[] = {
	{ .compatible = "espressif,esp32-spi" },
	{}
};
MODULE_DEVICE_TABLE(of, esp_spi_of_match);

static const struct spi_device_id esp_spi_id[] = {
	{ "esp32-spi", 0 },
	{}
};
MODULE_DEVICE_TABLE(spi, esp_spi_id);

static struct spi_driver esp_spi_driver = {
	.driver = {
		.name = "esp_spi",
		.of_match_table = esp_spi_of_match,
	},
	.probe = esp_spi_probe,
	.remove = esp_spi_remove,
	.id_table = esp_spi_id,
};

static int spi_init(void)
{
	int status;
	uint8_t prio_q_idx;

	spi_context.spi_workqueue =
		alloc_ordered_workqueue("ESP_SPI_WORK_QUEUE",
					WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (!spi_context.spi_workqueue) {
		esp_err("spi workqueue failed to create\n");
		return -ENOMEM;
	}
	INIT_WORK(&spi_context.spi_work, esp_spi_work);
	INIT_WORK(&spi_context.reset_work, esp_spi_reset_work);
	for (prio_q_idx = 0; prio_q_idx < MAX_PRIORITY_QUEUES; prio_q_idx++) {
		skb_queue_head_init(&spi_context.tx_q[prio_q_idx]);
		skb_queue_head_init(&spi_context.rx_q[prio_q_idx]);
	}

	status = spi_register_driver(&esp_spi_driver);
	if (status) {
		esp_err("Failed to register ESP SPI driver: %d\n", status);
		destroy_workqueue(spi_context.spi_workqueue);
		spi_context.spi_workqueue = NULL;
		return status;
	}
	spi_driver_registered = true;

	/*
	 * Do not require a synchronous probe here. Device Tree population,
	 * GPIO providers and the SPI controller may legitimately appear later
	 * or return -EPROBE_DEFER; the driver core owns retry semantics.
	 */
	return 0;
}

static void spi_exit(void)
{
	data_path = CLOSE_DATAPATH;

	if (spi_driver_registered) {
		spi_unregister_driver(&esp_spi_driver);
		spi_driver_registered = false;
	}
	if (spi_context.spi_workqueue) {
		cancel_work_sync(&spi_context.reset_work);
		cancel_work_sync(&spi_context.spi_work);
		esp_spi_purge_queues();
		destroy_workqueue(spi_context.spi_workqueue);
		spi_context.spi_workqueue = NULL;
	}
	memset(&spi_context, 0, sizeof(spi_context));
}

static void adjust_spi_clock(u8 spi_clk_mhz)
{
	u32 max_mhz;

	if (!spi_clk_mhz || !spi_context.esp_spi_dev)
		return;

	max_mhz = spi_context.spi_max_hz / NUMBER_1M;
	if (max_mhz && spi_clk_mhz > max_mhz) {
		esp_warn("Requested SPI clock %u MHz exceeds Device Tree cap %u Hz; capping to %u MHz\n",
			 spi_clk_mhz, spi_context.spi_max_hz, max_mhz);
		spi_clk_mhz = min_t(u32, max_mhz, 255U);
	}

	if (spi_clk_mhz != spi_context.spi_clk_mhz) {
		esp_info("ESP Reconfigure SPI CLK to %u MHz\n", spi_clk_mhz);
		spi_context.spi_clk_mhz = spi_clk_mhz;
		spi_context.esp_spi_dev->max_speed_hz =
			(u32)spi_clk_mhz * NUMBER_1M;
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

	memset(&spi_context, 0, sizeof(spi_context));
	adapter->if_context = &spi_context;
	adapter->if_ops = &if_ops;
	adapter->if_type = ESP_IF_TYPE_SPI;
	spi_context.adapter = adapter;
	if (speed > SPI_MAX_CLK_MHZ) {
		esp_warn("Requested SPI clock %u MHz exceeds protocol maximum %u MHz; capping\n",
			 speed, SPI_MAX_CLK_MHZ);
		speed = SPI_MAX_CLK_MHZ;
	}
	spi_context.requested_clk_mhz = speed ? speed : SPI_INITIAL_CLK_MHZ;
	spi_context.spi_clk_mhz = spi_context.requested_clk_mhz;
	return spi_init();
}

void esp_deinit_interface_layer(void)
{
	spi_exit();
}
