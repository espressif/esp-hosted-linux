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
#include "esp_host_wire_bounds.h"
#include "esp_stats.h"
#include "esp_utils.h"
#include "esp_cfg80211.h"
#include "esp_cmd.h"

#define SPI_INITIAL_CLK_MHZ     10
/* The first boot/ready event precedes firmware clock negotiation. */
#define SPI_BOOTSTRAP_CLK_MHZ      1
#define SPI_REBOOT_ACK_TIMEOUT_MS 2000
#define SPI_RESET_SETTLE_MS       500
#define SPI_REMOVE_RESET_SETTLE_MS 200
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
static int process_rx_buf(struct sk_buff *skb, bool rx_pending);

static volatile u8 data_path;
volatile u8 host_sleep;
static struct esp_spi_context spi_context;
static atomic_t tx_pending;
/* Incremented on every HANDSHAKE rising edge. A post-transfer worker may
 * still observe the previous slot's HANDSHAKE high until the slave's
 * post-transaction callback runs. Requeue only after a new generation proves
 * that the next DMA slot has actually been armed. */
static atomic_t spi_handshake_generation = ATOMIC_INIT(0);
static DEFINE_MUTEX(spi_reset_lock);
static bool spi_driver_registered;
static atomic_t spi_soft_reboot_attempts = ATOMIC_INIT(0);
/* Reboot attempts are scoped to the committed firmware incarnation. */
static DEFINE_SPINLOCK(spi_reboot_budget_lock);
static u32 spi_reboot_budget_epoch;
/* Boot-frame rescue is an initial-probe exception only. Once a firmware
 * incarnation has committed, an explicit DT hardware reset must not be
 * cancelled by an old/replayed BOOT frame still visible on the wire. */
static bool spi_boot_rescue_pending;

static void esp_spi_reboot_budget_sync_locked(struct esp_adapter *adapter)
{
	u32 epoch = (u32)atomic_read(&adapter->fw_boot_commit_epoch);

	if (spi_reboot_budget_epoch != epoch) {
		spi_reboot_budget_epoch = epoch;
		atomic_set(&spi_soft_reboot_attempts, 0);
	}
}
/* Distinct reboot generations reject ACKs retained from prior attempts. */
static atomic_t spi_reboot_cmd_seq = ATOMIC_INIT(0);

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

static bool esp_spi_next_slot_ready(bool handshake,
				    u32 handshake_generation_before,
				    u32 handshake_generation_now)
{
	/* A high HANDSHAKE by itself can still belong to the slot that just
	 * completed. Only a fresh rising-edge generation proves that firmware
	 * has armed the next DMA slot. */
	return handshake &&
	       handshake_generation_now != handshake_generation_before;
}

static void esp_spi_requeue_if_pending(u32 handshake_generation_before)
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
	/* spi_sync_transfer() may return before the slave-side post-transaction
	 * callback has lowered HANDSHAKE for the slot we just consumed. Never use
	 * that stale high level to start another transfer: doing so can clock the
	 * slave before its next DMA descriptor is armed and return an all-zero
	 * MISO frame while DATA_READY is asserted.
	 *
	 * A new HANDSHAKE rising-edge generation proves that firmware completed
	 * the low->high slot handoff. If that IRQ has not arrived yet, simply
	 * return; the IRQ handler will queue the worker once the next slot is
	 * actually ready. */
	if (!esp_spi_next_slot_ready(
			handshake, handshake_generation_before,
			(u32)atomic_read(&spi_handshake_generation)))
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
	atomic_set(&adapter->state, ESP_CONTEXT_READY);
	open_data_path();
	/* A parsed boot TLV is insufficient to refill the retry budget.
	 * WLAN/HCI/RCP publication still has to succeed. */
	return 0;
}

static void esp_spi_commit_fw_recovery(struct esp_adapter *adapter)
{
	unsigned long flags;

	/* The common epoch was advanced before invoking this callback. A new
	 * reset may have charged the next epoch already; never erase it. */
	spin_lock_irqsave(&spi_reboot_budget_lock, flags);
	esp_spi_reboot_budget_sync_locked(adapter);
	/* Boot-first rescue is only for the uncommitted incarnation present
	 * when the SPI device initially binds. */
	spi_boot_rescue_pending = false;
	spin_unlock_irqrestore(&spi_reboot_budget_lock, flags);
}

static bool esp_spi_claim_boot_rescue(void)
{
	unsigned long flags;
	bool rescue;

	spin_lock_irqsave(&spi_reboot_budget_lock, flags);
	rescue = spi_boot_rescue_pending;
	spi_boot_rescue_pending = false;
	spin_unlock_irqrestore(&spi_reboot_budget_lock, flags);
	return rescue;
}

static void esp_spi_arm_boot_rescue(void)
{
	unsigned long flags;

	spin_lock_irqsave(&spi_reboot_budget_lock, flags);
	spi_boot_rescue_pending = true;
	spin_unlock_irqrestore(&spi_reboot_budget_lock, flags);
}

static void esp_spi_retire_boot_rescue(void)
{
	unsigned long flags;

	spin_lock_irqsave(&spi_reboot_budget_lock, flags);
	spi_boot_rescue_pending = false;
	spin_unlock_irqrestore(&spi_reboot_budget_lock, flags);
}

static void esp_spi_reset_binding_budget(struct esp_adapter *adapter)
{
	unsigned long flags;

	spin_lock_irqsave(&spi_reboot_budget_lock, flags);
	spi_reboot_budget_epoch =
		adapter ? (u32)atomic_read(&adapter->fw_boot_commit_epoch) : 0;
	atomic_set(&spi_soft_reboot_attempts, 0);
	spi_boot_rescue_pending = false;
	spin_unlock_irqrestore(&spi_reboot_budget_lock, flags);
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

	/* The firmware always starts in its default SPI mode. The first
	 * boot event must be received at the safe bootstrap speed rather
	 * than at the last incarnation's negotiated throughput clock. */
	spi_context.spi_clk_mhz = SPI_BOOTSTRAP_CLK_MHZ;
	max_mhz = spi_context.spi_max_hz / NUMBER_1M;
	if (max_mhz && spi_context.spi_clk_mhz > max_mhz)
		spi_context.spi_clk_mhz = min_t(u32, max_mhz, 255U);
	if (spi_context.esp_spi_dev)
		spi_context.esp_spi_dev->max_speed_hz =
			(u32)spi_context.spi_clk_mhz * NUMBER_1M;
}

static int esp_spi_pulse_reset_gpio_locked(struct esp_spi_context *context,
					   unsigned int settle_ms,
					   const char *reason)
{
	int ret;

	if (!context || !context->reset_gpio)
		return -ENODEV;

	ret = gpiod_direction_output(context->reset_gpio, 0);
	if (ret) {
		esp_err("Failed to configure SPI reset GPIO as output: %d\n", ret);
		return ret;
	}
	context->reset_gpio_driven = true;

	esp_info("Resetting ESP SPI target (%s) via reset-gpios\n", reason);
	gpiod_set_value_cansleep(context->reset_gpio, 1);
	usleep_range(200, 500);
	gpiod_set_value_cansleep(context->reset_gpio, 0);
	ret = gpiod_direction_input(context->reset_gpio);
	if (ret) {
		esp_err("Failed to release SPI reset GPIO as input: %d\n", ret);
		return ret;
	}
	context->reset_gpio_driven = false;
	if (settle_ms)
		msleep(settle_ms);
	return 0;
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
	ret = esp_spi_pulse_reset_gpio_locked(&spi_context,
					      SPI_RESET_SETTLE_MS,
					      "recovery");
	if (ret)
		goto out;
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

/* Bootstrap reboot does not depend on a Wi-Fi device, the normal
 * command queue, or a negotiated SPI clock. It runs exclusively inside the
 * ordered SPI workqueue while ESP_SPI_RESETTING gates all normal traffic.
 *
 * A firmware ACK is meaningful only after a complete SPI transaction:
 * firmware's post-transaction worker delays esp_restart() until the master
 * has clocked the response off the wire. Unsupported old firmware times out
 * safely; absence of an ACK must never be reported as success.
 *
 * Return: 0 = acknowledged reboot, 1 = valid pending boot event (no reboot),
 * <0 = transport error / no explicit command response.
 */
/* Validate a complete bootstrap wire frame before classifying it as a
 * boot event or accepting an ACK. Header ID alone is never sufficient. */
static bool esp_spi_bootstrap_frame_valid(struct esp_adapter *adapter,
					u8 *frame, bool boot, u16 *payload_len)
{
	struct esp_payload_header *hdr = (void *)frame;
	struct esp_internal_bootup_event *evt;
	u16 off = esp_wire_le16_to_cpu(hdr->offset);
	u16 len = esp_wire_le16_to_cpu(hdr->len);
	u16 rx_sum = esp_wire_le16_to_cpu(hdr->checksum);
	/* CMD35 exists only in new firmware, which always checksums SPI
	 * responses, even before boot capability negotiation. Do not accept an
	 * unchecked all-zero checksum on the privileged reboot ACK path. */
	bool needs_sum = !boot || rx_sum;
	bool cap = false, chip = false, fw = false, clk = false;
	u16 calc;
	const u8 *pos;
	int left;

	(void)adapter;
	if (hdr->if_type != ESP_INTERNAL_IF || hdr->if_num != 0 ||
	    hdr->flags != 0 || hdr->reserved1 != 0 ||
	    hdr->reserved2 != 0 || hdr->reserved3 != 0 ||
	    !ESP_OFFSET_VALID(off) ||
	    off > SPI_BUF_SIZE || !len || len > SPI_BUF_SIZE - off)
		return false;
	if (boot) {
		if (off != sizeof(*hdr) ||
		    (hdr->packet_type != PACKET_TYPE_EVENT &&
		     hdr->packet_type != PACKET_TYPE_DATA) ||
		    len < offsetof(struct esp_internal_bootup_event, data))
			return false;
		evt = (void *)(frame + off);
		if (evt->header.event_code != ESP_INTERNAL_BOOTUP_EVENT ||
		    evt->header.status ||
		    len != offsetof(struct esp_internal_bootup_event, data) +
		           evt->len ||
		    esp_wire_le16_to_cpu(evt->header.len) != (u16)evt->len + 1)
			return false;
		pos = evt->data;
		left = evt->len;
		while (left) {
			u8 n, tag;

			if (left < 2)
				return false;
			tag = pos[0];
			n = pos[1];
			if (n > left - 2)
				return false;
			switch (tag) {
			case ESP_BOOTUP_CAPABILITY:
				if (cap || (n != 1 && n != 2 && n != 4))
					return false;
				{
					u32 capability = 0;

					if (n == 1) {
						capability = pos[2];
					} else if (n == 2) {
						__le16 cap16;

						memcpy(&cap16, pos + 2, sizeof(cap16));
						capability = le16_to_cpu(cap16);
					} else {
						__le32 cap32;

						memcpy(&cap32, pos + 2, sizeof(cap32));
						capability = le32_to_cpu(cap32);
					}
					if (!(capability & ESP_WLAN_SPI_SUPPORT))
						return false;
					if (capability & ESP_CHECKSUM_ENABLED)
						needs_sum = true;
				}
				cap = true;
				break;
			case ESP_BOOTUP_FIRMWARE_CHIP_ID:
				if (chip || n != 1 ||
				    pos[2] == ESP_FIRMWARE_CHIP_UNRECOGNIZED)
					return false;
				chip = true;
				break;
			case ESP_BOOTUP_FW_DATA:
				if (fw || n != sizeof(struct fw_data))
					return false;
				fw = true;
				break;
			case ESP_BOOTUP_SPI_CLK_MHZ:
				if (clk || n != 1 || !pos[2])
					return false;
				clk = true;
				break;
			case ESP_BOOTUP_EXT_CAPABILITY:
			case ESP_BOOTUP_RX_BUF_SIZE:
				if (n != sizeof(u32))
					return false;
				break;
			case ESP_BOOTUP_RCP_SESSION_NONCE:
				if (n != sizeof(u64))
					return false;
				break;
			default:
				break;
			}
			pos += 2 + n;
			left -= 2 + n;
		}
		if (!cap || !chip || !fw)
			return false;
	} else if (hdr->packet_type != PACKET_TYPE_COMMAND_RESPONSE ||
		   len < sizeof(struct command_header)) {
		return false;
	}
	if (needs_sum) {
		hdr->checksum = 0;
		calc = compute_checksum(frame, off + len);
		hdr->checksum = esp_wire_cpu_to_le16(rx_sum);
		if (calc != rx_sum)
			return false;
	}
	*payload_len = len;
	return true;
}

static int esp_spi_bootstrap_check_boot(struct esp_adapter *adapter,
					struct sk_buff **pending_boot,
					bool allow_firmware_reboot)
{
	struct spi_transfer trans = {};
	struct esp_payload_header *wire;
	struct command_header *cmd;
	struct sk_buff *boot_skb = NULL;
	u8 *tx, *rx;
	unsigned long deadline;
	bool sent = false;
	bool ready, data_ready;
	u16 reboot_seq = 0;
	u16 frame_len = 0;
	int ret = -ETIMEDOUT;
	int n = 0;

	if (!adapter || !spi_context.esp_spi_dev || !pending_boot)
		return -ENODEV;
	*pending_boot = NULL;
	if (allow_firmware_reboot) {
		/* Never reuse the same ACK sequence on consecutive attempts.
		 * An old already-clocked response must not authorize a new reboot. */
		reboot_seq = (u16)atomic_inc_return(&spi_reboot_cmd_seq);
		if (!reboot_seq)
			reboot_seq = (u16)atomic_inc_return(&spi_reboot_cmd_seq);
	}
	tx = kzalloc(SPI_BUF_SIZE, GFP_KERNEL);
	rx = kzalloc(SPI_BUF_SIZE, GFP_KERNEL);
	if (!tx || !rx) {
		ret = -ENOMEM;
		goto out;
	}
	/* A slow bootstrap clock is required: there is no way to obtain
	 * data over SPI without the Linux master supplying SCLK. */
	spi_context.spi_clk_mhz = SPI_BOOTSTRAP_CLK_MHZ;
	spi_context.esp_spi_dev->max_speed_hz =
		SPI_BOOTSTRAP_CLK_MHZ * NUMBER_1M;
	deadline = jiffies + msecs_to_jiffies(allow_firmware_reboot ?
				 SPI_REBOOT_ACK_TIMEOUT_MS : 400);
	while (time_before(jiffies, deadline)) {
		ret = esp_spi_gpio_asserted(spi_context.handshake_gpio,
					      "handshake", &ready);
		if (ret)
			goto out;
		ret = esp_spi_gpio_asserted(spi_context.data_ready_gpio,
					      "data-ready", &data_ready);
		if (ret)
			goto out;
		if (!ready) {
			msleep(10);
			continue;
		}
		if (!allow_firmware_reboot && !data_ready) {
			/* No boot event is queued on the slave; avoid clocking
			 * dummy 0xFF transactions before physical GPIO reset. */
			break;
		}

		memset(tx, 0, SPI_BUF_SIZE);
		memset(rx, 0, SPI_BUF_SIZE);
		/* Inspect any already-advertised event before injecting a reboot.
		 * The firmware may have queued BOOT before Linux bound its IRQ. */
		if (allow_firmware_reboot && !sent && (!data_ready || n++ >= 8)) {
			wire = (struct esp_payload_header *)tx;
			wire->if_type = ESP_INTERNAL_IF;
			wire->packet_type = PACKET_TYPE_COMMAND_REQUEST;
			wire->offset = esp_wire_cpu_to_le16(sizeof(*wire));
			wire->len = esp_wire_cpu_to_le16(sizeof(*cmd));
			cmd = (struct command_header *)(tx + sizeof(*wire));
			cmd->cmd_code = CMD_HOST_REBOOT;
			cmd->seq_num = esp_wire_cpu_to_le16(reboot_seq);
			wire->checksum = esp_wire_cpu_to_le16(
				compute_checksum(tx, sizeof(*wire) + sizeof(*cmd)));
			sent = true;
			esp_info("SPI software reboot requested at %u MHz seq=%u; awaiting wire ACK\n",
				 SPI_BOOTSTRAP_CLK_MHZ, reboot_seq);
		}
		trans.tx_buf = tx;
		trans.rx_buf = rx;
		trans.len = SPI_BUF_SIZE;
		trans.speed_hz = SPI_BOOTSTRAP_CLK_MHZ * NUMBER_1M;
		ret = spi_sync_transfer(spi_context.esp_spi_dev, &trans, 1);
		if (ret)
			goto out;

		wire = (struct esp_payload_header *)rx;
		if (!sent && wire->if_type == ESP_INTERNAL_IF &&
		    (wire->packet_type == PACKET_TYPE_EVENT ||
		     wire->packet_type == PACKET_TYPE_DATA) &&
		    esp_spi_bootstrap_frame_valid(adapter, rx, true,
							  &frame_len)) {
			/* Preserve the published event and let the common boot event
			 * worker validate TLVs and reconstruct interfaces. */
			boot_skb = esp_spi_alloc_skb(SPI_BUF_SIZE);
			if (!boot_skb) {
				ret = -ENOMEM;
				goto out;
			}
			skb_put_data(boot_skb, rx, SPI_BUF_SIZE);
			*pending_boot = boot_skb;
			ret = 1;
			goto out;
		}
		if (sent && wire->if_type == ESP_INTERNAL_IF &&
		    wire->packet_type == PACKET_TYPE_COMMAND_RESPONSE &&
		    esp_spi_bootstrap_frame_valid(adapter, rx, false,
							  &frame_len)) {
			cmd = (struct command_header *)(rx +
					esp_wire_le16_to_cpu(wire->offset));
			if (frame_len == sizeof(*cmd) &&
			    esp_wire_le16_to_cpu(cmd->len) == 0 &&
			    cmd->reserved1 == 0 && cmd->reserved2 == 0 &&
			    cmd->cmd_code == CMD_HOST_REBOOT &&
			    esp_wire_le16_to_cpu(cmd->seq_num) == reboot_seq) {
				ret = cmd->cmd_status == CMD_RESPONSE_SUCCESS ? 0 : -EIO;
				goto out;
			}
		}
		msleep(5);
	}
	ret = allow_firmware_reboot ? -ETIMEDOUT : -ENODATA;
out:
	kfree(rx);
	kfree(tx);
	return ret;
}

static void esp_spi_reset_work(struct work_struct *work)
{
	struct esp_spi_context *context =
		container_of(work, struct esp_spi_context, reset_work);
	struct esp_adapter *adapter = context->adapter;
	struct sk_buff *pending_boot = NULL;
	int ret;

	if (context->reset_gpio) {
		/* Boot-first rescue applies only while the initial probe has never
		 * committed an incarnation. Once Linux explicitly decides a running
		 * incarnation must die, a queued/replayed BOOT frame cannot cancel
		 * the requested physical reset. */
		if (esp_spi_claim_boot_rescue())
			ret = esp_spi_bootstrap_check_boot(adapter, &pending_boot, false);
		else
			ret = -ENODATA;
		if (ret != 1)
			ret = esp_spi_hw_reset(adapter);
	} else {
		ret = esp_spi_bootstrap_check_boot(adapter, &pending_boot, true);
	}
	/* Do not reset the attempt budget on a command ACK: the firmware
	 * could acknowledge repeatedly yet never deliver a fresh boot event.
	 * The accepted boot event (reinit callback) releases the budget. */
	if (ret < 0) {
		if (ret != -ESHUTDOWN && adapter) {
			set_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);
			esp_err("SPI target reset failed: %d\n", ret);
		}
	} else if (adapter) {
		clear_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);
	}

	clear_bit(ESP_SPI_RESETTING, &context->spi_flags);
	if (ret == 1 && pending_boot) {
		/* Boot-event-first has already checked complete wire bounds,
		 * checksum and required boot TLVs. Enqueue can still fail. */
		data_path = OPEN_DATAPATH;
		if (process_rx_buf(pending_boot, true)) {
			dev_kfree_skb(pending_boot);
			set_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);
			esp_err("SPI pending boot enqueue failed; preserving reset intent\n");
		}
	} else if (ret == 0 && !context->reset_gpio) {
		/* The ACK only proves that firmware accepted CMD35. Its post-transfer
		 * task deliberately restarts a little later. Keep the ordinary datapath
		 * closed during that interval so old-incarnation HCI/RCP/internal frames
		 * cannot cross the reincarnation boundary. recover_transport() reopens
		 * SPI when it is time to wait for the fresh BOOT TLV. */
		esp_info("SPI CMD_HOST_REBOOT wire ACK accepted; holding datapath closed for new boot TLV\n");
		data_path = CLOSE_DATAPATH;
	}

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
	unsigned long flags;
	u32 attempt_epoch;
	bool software_reset;

	if (!adapter || adapter != spi_context.adapter)
		return -EINVAL;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return -ESHUTDOWN;
	if (!spi_context.spi_workqueue)
		return -ENODEV;

	/* A delayed commit callback must not erase a new incarnation's reboot,
	 * or let an inherited count reject its first attempt. Serialize both
	 * the generation check and the in-flight reset gate. */
	spin_lock_irqsave(&spi_reboot_budget_lock, flags);
	esp_spi_reboot_budget_sync_locked(adapter);
	if (test_bit(ESP_SPI_RESETTING, &spi_context.spi_flags)) {
		spin_unlock_irqrestore(&spi_reboot_budget_lock, flags);
		return 0;
	}
	software_reset = !spi_context.reset_gpio;
	if (software_reset &&
	    atomic_read(&spi_soft_reboot_attempts) >= ESP_FW_BOOT_MAX_ATTEMPTS) {
		spin_unlock_irqrestore(&spi_reboot_budget_lock, flags);
		return -EHOSTUNREACH;
	}
	set_bit(ESP_SPI_RESETTING, &spi_context.spi_flags);
	attempt_epoch = spi_reboot_budget_epoch;
	if (software_reset)
		atomic_inc(&spi_soft_reboot_attempts);
	spin_unlock_irqrestore(&spi_reboot_budget_lock, flags);

	data_path = CLOSE_DATAPATH;
	if (atomic_read(&adapter->state) > ESP_CONTEXT_RX_READY)
		atomic_set(&adapter->state, ESP_CONTEXT_RX_READY);

	if (!queue_work(spi_context.spi_workqueue, &spi_context.reset_work)) {
		spin_lock_irqsave(&spi_reboot_budget_lock, flags);
		clear_bit(ESP_SPI_RESETTING, &spi_context.spi_flags);
		/* No new reset was queued. Roll back only the current epoch. */
		if (software_reset && spi_reboot_budget_epoch == attempt_epoch &&
		    atomic_read(&spi_soft_reboot_attempts) > 0)
			atomic_dec(&spi_soft_reboot_attempts);
		spin_unlock_irqrestore(&spi_reboot_budget_lock, flags);
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
	.commit_fw_recovery = esp_spi_commit_fw_recovery,
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
	atomic_inc(&spi_handshake_generation);
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

static bool spi_is_dummy_rx(const struct esp_payload_header *header)
{
	/*
	 * Firmware deliberately arms this canonical frame when no E2H payload is
	 * ready. data-ready may subsequently rise for a newly queued payload while
	 * the already-armed dummy is still the transaction the host will clock.
	 * Treat only the exact firmware sentinel as no-data; any other malformed
	 * frame remains fail-closed while RCP is active.
	 */
	return header &&
		header->if_type == 0xF &&
		header->if_num == 0xF &&
		header->flags == 0 &&
		header->packet_type == 0 &&
		header->reserved1 == 0 &&
		esp_wire_le16_to_cpu(header->len) == 0 &&
		esp_wire_le16_to_cpu(header->offset) == 0 &&
		esp_wire_le16_to_cpu(header->checksum) == 0 &&
		header->reserved2 == 0 &&
		header->reserved3 == 0;
}

static bool spi_is_unadvertised_idle_rx(const struct sk_buff *skb,
		bool rx_pending)
{
	u32 i;

	/*
	 * A host-only H2E transfer can race the slave arming its next dummy
	 * transaction. When data-ready was not asserted, an entirely zero MISO
	 * transaction carries no advertised E2H ownership and is transport idle.
	 * Keep this exception narrower than the canonical dummy rule: once
	 * data-ready advertised RX, any non-canonical zero/malformed frame remains
	 * stateful-fatal.
	 */
	if (rx_pending || !skb || skb->len != SPI_BUF_SIZE)
		return false;

	for (i = 0; i < skb->len; i++) {
		if (skb->data[i] != 0)
			return false;
	}

	return true;
}

static int process_rx_buf(struct sk_buff *skb, bool rx_pending)
{
	struct esp_payload_header *header;
	u16 len = 0;
	u16 offset = 0;

	if (!skb || !skb->data || skb->len < sizeof(*header))
		return -EINVAL;
	header = (struct esp_payload_header *)skb->data;
	if (spi_is_dummy_rx(header) ||
	    spi_is_unadvertised_idle_rx(skb, rx_pending))
		return -ENODATA;
	if (header->if_type >= ESP_MAX_IF) {
		esp_spi_fail_rcp_rx("invalid interface type");
		return -EINVAL;
	}

	offset = esp_wire_le16_to_cpu(header->offset);
	len = esp_wire_le16_to_cpu(header->len);
	if (len == 0) {
		esp_err("Drop zero SPI RX payload: if=%u if_num=%u type=%u flags=0x%02x "
			"offset=%u checksum=0x%04x rx_pending=%u\n",
			header->if_type, header->if_num, header->packet_type,
			header->flags, offset,
			esp_wire_le16_to_cpu(header->checksum), rx_pending);
		esp_spi_fail_rcp_rx("zero payload");
		return -EINVAL;
	}
	if (len > SPI_BUF_SIZE ||
	    !esp_host_wire_span_valid(len, offset, skb->len, NULL)) {
		esp_err("Drop invalid pkt: len=%d offset=%d skb_len=%u\n", len, offset, skb->len);
		esp_spi_fail_rcp_rx("invalid length/offset");
		return -EINVAL;
	}
	if (header->if_type == ESP_INTERNAL_IF) {
		u16 frame_len = 0;
		bool is_boot_evt = len >= sizeof(struct event_header) &&
			((const struct event_header *)(skb->data + offset))->event_code ==
			ESP_INTERNAL_BOOTUP_EVENT;

		if (header->if_num != 0 || header->flags != 0 ||
		    header->reserved1 != 0 || header->reserved2 != 0 ||
		    header->reserved3 != 0) {
			esp_err("Drop non-canonical internal SPI RX: if_num=%u type=%u flags=0x%02x\n",
				header->if_num, header->packet_type,
				header->flags);
			esp_spi_fail_rcp_rx("non-canonical internal header");
			return -EINVAL;
		}
		if (is_boot_evt ||
		    header->packet_type == PACKET_TYPE_EVENT ||
		    header->packet_type == PACKET_TYPE_DATA) {
			if (!esp_spi_bootstrap_frame_valid(spi_context.adapter,
							   skb->data, true,
							   &frame_len)) {
				esp_err("Drop invalid SPI boot RX frame: type=%u len=%u off=%u\n",
					header->packet_type, len, offset);
				esp_spi_fail_rcp_rx("invalid boot frame");
				return -EINVAL;
			}
			/* The initial boot-first exception ends as soon as a
			 * validated BOOT frame is consumed from the wire. If
			 * reconstruction later fails, the ensuing explicit
			 * hardware reset is authoritative and cannot be
			 * cancelled by a stale/replayed BOOT frame. */
			esp_spi_retire_boot_rescue();
		} else if (header->packet_type != PACKET_TYPE_COMMAND_RESPONSE ||
			   !esp_spi_bootstrap_frame_valid(spi_context.adapter,
							  skb->data, false,
							  &frame_len)) {
			esp_err("Drop invalid internal SPI RX frame: type=%u len=%u off=%u\n",
				header->packet_type, len, offset);
			esp_spi_fail_rcp_rx("invalid internal frame");
			return -EINVAL;
		}
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
	u32 handshake_generation_before;

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
	handshake_generation_before =
		(u32)atomic_read(&spi_handshake_generation);

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
		if (process_rx_buf(rx_skb, rx_pending))
			dev_kfree_skb(rx_skb);
		dev_kfree_skb(tx_skb);
		esp_spi_requeue_if_pending(handshake_generation_before);
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

	/* clockspeed= is the historical initial/pre-negotiation host rate, not
	 * a permanent ceiling on the firmware-advertised operating clock. Keep
	 * the configured value only so a firmware that omits the optional clock
	 * TLV can return from the 1 MHz boot-safe rate to the old startup rate. */
	requested_hz = (u32)spi_context.requested_clk_mhz * NUMBER_1M;
	if (spi_context.requested_clk_mhz &&
	    requested_hz > spi_context.spi_max_hz)
		esp_warn("Initial SPI clock %u MHz exceeds Device Tree cap %u Hz; fallback will be limited to %u MHz\n",
			 spi_context.requested_clk_mhz, spi_context.spi_max_hz, max_mhz);
	/* Clock the pending boot event at 1 MHz regardless of the requested
	 * steady-state rate. The validated boot TLV triggers negotiation. */
	spi_context.spi_clk_mhz = SPI_BOOTSTRAP_CLK_MHZ;

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

	/* EN is optional: an already-running ESP can publish its boot event
	 * without ever being reset. Only reset if the bounded boot probe fails. */
	spi_context.reset_gpio =
		devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_IN);
	if (IS_ERR(spi_context.reset_gpio)) {
		status = PTR_ERR(spi_context.reset_gpio);
		spi_context.reset_gpio = NULL;
		esp_err("Failed to get reset GPIO: %d\n", status);
		goto err_clear_dev;
	}
	/*
	 * A board can keep the ESP off until the host enables it, with a
	 * pull-down on EN. The released (input) line then holds the ESP in
	 * reset, and no boot event can come: enable the ESP now instead of
	 * after the boot event timeout.
	 */
	if (spi_context.reset_gpio &&
	    gpiod_get_value_cansleep(spi_context.reset_gpio) == 1) {
		mutex_lock(&spi_reset_lock);
		status = gpiod_direction_output(spi_context.reset_gpio, 0);
		if (!status)
			spi_context.reset_gpio_driven = true;
		mutex_unlock(&spi_reset_lock);
		if (status) {
			esp_err("Failed to release the reset GPIO: %d\n", status);
			goto err_clear_dev;
		}
		esp_info("ESP held in reset by its EN line, releasing it\n");
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
	 * Do not reset during probe: firmware may already have a valid boot
	 * event queued on the data-ready line. Probe it first at 1 MHz; the
	 * common watchdog requests hardware reset only if no boot event arrives.
	 * A reset GPIO, when described in DT, remains available for that path.
	 */
	clear_bit(ESP_TRANSPORT_REMOVING, &spi_context.adapter->state_flags);
	clear_bit(ESP_CLEANUP_IN_PROGRESS, &spi_context.adapter->state_flags);
	esp_info("SPI boot event probe at %u MHz (DT reset %s)\n",
		 SPI_BOOTSTRAP_CLK_MHZ,
		 spi_context.reset_gpio ? "available" : "absent");

	/* A reprobe is a fresh host binding, not a continuation of an exhausted
	 * recovery episode. Reset both common and SPI-local retry budgets before
	 * granting the one initial boot-first rescue token. */
	esp_reset_boot_recovery_for_binding(spi_context.adapter, false);
	esp_spi_reset_binding_budget(spi_context.adapter);
	esp_spi_arm_boot_rescue();
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

	/*
	 * The ESP SPI slave queues its boot-up event only once at startup and
	 * has no in-band close-datapath interrupt on SPI. When a hardware
	 * reset line is described in DT, pulse it on module unload/unbind so
	 * the target reboots and arms a fresh boot-up event before the next
	 * insmod probes.
	 */
	mutex_lock(&spi_reset_lock);
	if (context->reset_gpio)
		esp_spi_pulse_reset_gpio_locked(context,
						SPI_REMOVE_RESET_SETTLE_MS,
						"remove");
	mutex_unlock(&spi_reset_lock);

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
	atomic_set(&spi_handshake_generation, 0);
	atomic_set(&spi_soft_reboot_attempts, 0);
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

	/* Preserve master-compatible semantics: a valid firmware boot TLV owns
	 * the steady-state rate. clockspeed= only selected the pre-negotiation
	 * startup/fallback rate; Device Tree remains the board safety ceiling. */
	if (spi_clk_mhz != spi_context.spi_clk_mhz) {
		esp_info("ESP Reconfigure SPI CLK to %u MHz\n", spi_clk_mhz);
		spi_context.spi_clk_mhz = spi_clk_mhz;
		spi_context.esp_spi_dev->max_speed_hz =
			(u32)spi_clk_mhz * NUMBER_1M;
	}
}

int esp_adjust_spi_clock(struct esp_adapter *adapter, u8 spi_clk_mhz)
{
	if (!adapter || adapter != spi_context.adapter)
		return -EINVAL;

	/* Internal zero means the optional BOOT clock TLV was absent. Restore the
	 * historical initial host rate instead of leaving recovery/bootstrap at
	 * 1 MHz forever. A real firmware TLV of zero is rejected by main.c. */
	if (!spi_clk_mhz)
		spi_clk_mhz = spi_context.requested_clk_mhz ?
			spi_context.requested_clk_mhz : SPI_INITIAL_CLK_MHZ;
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
	/* Preserve master semantics: clockspeed= selects only the initial host
	 * rate. The BOOT clock TLV may later select a higher operating rate.
	 * Store zero as "use historical 10 MHz fallback" after boot-safe receive. */
	spi_context.requested_clk_mhz = speed;
	spi_context.spi_clk_mhz = speed ? speed : SPI_INITIAL_CLK_MHZ;
	return spi_init();
}

void esp_deinit_interface_layer(void)
{
	spi_exit();
}
