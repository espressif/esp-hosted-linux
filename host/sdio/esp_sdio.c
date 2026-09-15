// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 */
#include "utils.h"
#include <linux/mutex.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "esp_if.h"
#include "esp_sdio_api.h"
#include "esp_bt_api.h"
#include "esp_cmd.h"
#include "esp_api.h"
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include "esp_stats.h"
#include "esp_utils.h"
#include "esp_kernel_port.h"

#define MAX_WRITE_RETRIES       2000
#define TX_MAX_PENDING_COUNT    1000
#define TX_RESUME_THRESHOLD     (TX_MAX_PENDING_COUNT/5)
#define ESP_SDIO_ALIGN_4(len)   (((len) + 3) & ~3)
#define ESP_SDIO_TX_STALL_WARN_MS 5000
#define ESP_SDIO_PKT_LEN_ZERO_RETRIES 20
#define ESP_SDIO_PKT_LEN_RETRY_MIN_US  20
#define ESP_SDIO_PKT_LEN_RETRY_MAX_US  50
#define ESP_SDIO_PKT_LEN_DELAYED_RETRIES 32

#define CHECK_SDIO_RW_ERROR(ret) do {                                     \
	if (ret)                                                            \
		esp_err("ESP_SDIO_CMD53_ERROR: ret=%d line=%d\n",          \
			(ret), __LINE__);                                      \
} while (0);

/* Persistent CMD53 buffers. GFP_DMA constrains placement on platforms whose
 * MMC host cannot map highmem; keep using the same allocator for ISR/TX
 * aggregates. 4-byte register reads reuse these buffers. */
#if defined(CONFIG_ZONE_DMA32)
#define ESP_SDIO_DMA_GFP (GFP_KERNEL | GFP_DMA32)
#else
#define ESP_SDIO_DMA_GFP (GFP_KERNEL | GFP_DMA)
#endif

static struct esp_sdio_context sdio_context;
static atomic_t tx_pending;
static atomic_t queue_items[MAX_PRIORITY_QUEUES];
static u32 sdio_buf_available;
#ifdef ESP_DEBUG_STATS
static atomic_t h2e_host_tx_queued;
static atomic_t h2e_host_tx_sent;
static atomic_t h2e_host_drop_queue_full;
static atomic_t h2e_host_drop_invalid;
static atomic_t h2e_host_drop_truncated;
static atomic_t h2e_host_no_credit_waits;
static atomic_t h2e_host_write_fail;
static unsigned long h2e_host_stats_jiffies;
static int h2e_host_last_sent;
static u64 h2e_host_time_write_us;
static u64 h2e_host_time_credit_us;
static u64 h2e_host_time_aggr_us;
#endif
static struct task_struct *tx_thread;
volatile u8 host_sleep;
static atomic_t tx_in_flight = ATOMIC_INIT(0);
static atomic_t sdio_need_counter_rebase;
static atomic_t sdio_need_slave_reset;
static u8 sdio_reset_gen;

static int tx_process(void *data);
static int get_firmware_data(struct esp_sdio_context *context);
static int init_context(struct esp_sdio_context *context);
static struct sk_buff *read_packet(struct esp_adapter *adapter);
static int write_packet(struct esp_adapter *adapter, struct sk_buff *skb);
static void sdio_purge_tx_queues(struct esp_sdio_context *context);
static bool sdio_get_cmd_info(struct sk_buff *skb, u8 *cmd_code, u16 *cmd_seq);


static void esp_sdio_request_transport_recovery(void);
static void esp_sdio_request_fw_reset_recovery(void);
static void esp_sdio_request_slave_reset(void);
static void esp_sdio_note_fw_reset(struct esp_adapter *adapter);

static void esp_sdio_free_dma_bufs(struct esp_sdio_context *context)
{
	if (!context)
		return;
	kfree(context->reg_buf);
	kfree(context->rx_len_buf);
	kfree(context->token_buf);
	kfree(context->tx_aggr_buf);
	context->reg_buf = NULL;
	context->rx_len_buf = NULL;
	context->token_buf = NULL;
	context->tx_aggr_buf = NULL;
}

#ifdef ESP_DEBUG_STATS
#define H2E_HOST_STATS_INC(counter) atomic_inc(&(counter))
#define H2E_HOST_STATS_ADD(counter, value) atomic_add((value), &(counter))
#define H2E_HOST_STATS_TIME_ADD(counter, start_time) \
	do { \
		(counter) += ktime_to_us(ktime_sub(ktime_get(), start_time)); \
	} while (0)

static void print_h2e_host_stats(void)
{
	unsigned long now = jiffies;
	int queued;
	int sent;
	int sent_delta;
	u64 avg_write;
	u64 avg_credit;
	u64 avg_aggr;

	if (time_before(now, h2e_host_stats_jiffies + 5 * HZ))
		return;

	h2e_host_stats_jiffies = now;
	queued = atomic_read(&h2e_host_tx_queued);
	sent = atomic_read(&h2e_host_tx_sent);
	sent_delta = sent - h2e_host_last_sent;
	h2e_host_last_sent = sent;
	if (queued ||
	    sent ||
	    atomic_read(&h2e_host_drop_queue_full) ||
	    atomic_read(&h2e_host_drop_invalid) ||
	    atomic_read(&h2e_host_drop_truncated) ||
	    atomic_read(&h2e_host_no_credit_waits) ||
	    atomic_read(&h2e_host_write_fail)) {
		avg_write = sent_delta ? div_u64(h2e_host_time_write_us,
				sent_delta) : 0;
		avg_credit = sent_delta ? div_u64(h2e_host_time_credit_us,
				sent_delta) : 0;
		avg_aggr = sent_delta ? div_u64(h2e_host_time_aggr_us,
				sent_delta) : 0;
		h2e_host_time_write_us = 0;
		h2e_host_time_credit_us = 0;
		h2e_host_time_aggr_us = 0;
		esp_info("H2E host stats cumulative: queued_frames=%d sent_frames=%d "
			 "uncommitted=%d tx_pending=%d qfull=%d invalid=%d "
			 "truncated=%d no_credit=%d write_fail_aggr=%d "
			 "avg_us_per_frame(write/credit/aggr)=%llu/%llu/%llu\n",
			 queued, sent, queued - sent, atomic_read(&tx_pending),
			 atomic_read(&h2e_host_drop_queue_full),
			 atomic_read(&h2e_host_drop_invalid),
			 atomic_read(&h2e_host_drop_truncated),
			 atomic_read(&h2e_host_no_credit_waits),
			 atomic_read(&h2e_host_write_fail),
			 avg_write, avg_credit, avg_aggr);
	}
}
#else
#define H2E_HOST_STATS_INC(counter) do { } while (0)
#define H2E_HOST_STATS_ADD(counter, value) do { } while (0)
#define H2E_HOST_STATS_TIME_ADD(counter, start_time) do { } while (0)
static inline void print_h2e_host_stats(void) { }
#endif

static const struct sdio_device_id esp_devices[] = {
	{ SDIO_DEVICE(ESP_VENDOR_ID_1, ESP_DEVICE_ID_ESP32_1) },
	{ SDIO_DEVICE(ESP_VENDOR_ID_2, ESP_DEVICE_ID_C5_C6_C61_1) },
	{}
};

static void esp_process_interrupt(struct esp_sdio_context *context)
{
	if (!context || !context->adapter)
		return;

	/* A function IRQ is itself sufficient reason to sample authoritative
	 * PACKET_LEN once. rx_pending separately controls publication retries. */
	esp_process_new_packet_intr(context->adapter);
}

static void esp_sdio_rx_len_retry_work(struct work_struct *work)
{
	struct esp_sdio_context *context = container_of(work,
			struct esp_sdio_context, rx_len_retry_work.work);
	struct esp_adapter *adapter = context->adapter;

	if (!adapter ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
	    (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags)) ||
	    atomic_read(&adapter->state) < ESP_CONTEXT_RX_READY)
		return;

	esp_process_new_packet_intr(adapter);
}

static void esp_handle_isr(struct sdio_func *func)
{
	struct esp_sdio_context *context = NULL;
	u32 int_status = 0;
	int ret;

	if (!func) {
		return;
	}

	if (host_sleep)
		return;

	context = sdio_get_drvdata(func);

	if (!(context) ||
	    !(context->adapter) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &context->adapter->state_flags) ||
	    (test_bit(ESP_DRIVER_UNLOADING, &context->adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_DEINIT, &context->adapter->state_flags)) ||
	    (atomic_read(&context->adapter->state) < ESP_CONTEXT_RX_READY)) {
		return;
	}

	if (!context->reg_buf)
		return;

	/* sdio_claim_irq() invokes this with the host already claimed. Nested
	 * sdio_claim_host() here can start a second CMD53 while Host→ESP DMA is
	 * being programmed and Oops in bcm2835_mmc_transfer_dma. */
	ret = esp_read_reg(context, ESP_SLAVE_INT_ST_REG,
			(u8 *)context->reg_buf, sizeof(u32),
			LOCK_ALREADY_ACQUIRED);
	if (ret) {
		CHECK_SDIO_RW_ERROR(ret);
		esp_sdio_request_transport_recovery();
		return;
	}

	int_status = *context->reg_buf;
	if (int_status & ESP_SLAVE_RX_NEW_PACKET_INT) {
		atomic_set(&context->rx_pending, 1);
	}

	ret = esp_write_reg(context, ESP_SLAVE_INT_CLR_REG,
			(u8 *)context->reg_buf, sizeof(*context->reg_buf),
			LOCK_ALREADY_ACQUIRED);
	CHECK_SDIO_RW_ERROR(ret);

	esp_process_interrupt(context);
}

int generate_slave_intr(void *if_context, u8 data)
{
	struct esp_sdio_context *context = if_context;
	u8 *val;
	int ret = 0;

	if (!context || !context->func)
		return -EINVAL;

	val = kmalloc(sizeof(u8), GFP_KERNEL);

	if (!val) {
		return -ENOMEM;
	}

	*val = data;

	ret = esp_write_reg(context, ESP_SLAVE_SCRATCH_REG_7, val,
			sizeof(*val), ACQUIRE_LOCK);

	kfree(val);

	return ret;
}

static void deinit_sdio_func(struct esp_sdio_context *context)
{
	struct sdio_func *func;

	if (!context || !context->func)
		return;

	func = context->func;
	sdio_set_drvdata(func, NULL);
	sdio_claim_host(func);
	if (context->irq_claimed) {
		sdio_release_irq(func);
		context->irq_claimed = false;
	}
	sdio_disable_func(func);
	sdio_release_host(func);
}

static int esp_sdio_read_u32(struct esp_sdio_context *context, u32 reg,
			     u32 *dma_buf, u32 *out, u8 is_lock_needed)
{
	int ret;

	if (!context || !context->func || !dma_buf || !out)
		return -EINVAL;

	if (is_lock_needed)
		sdio_claim_host(context->func);

	ret = esp_read_reg(context, reg, (u8 *)dma_buf, sizeof(u32),
			   LOCK_ALREADY_ACQUIRED);
	if (!ret)
		*out = *dma_buf;

	if (is_lock_needed)
		sdio_release_host(context->func);

	return ret;
}

/* Establish the Host→ESP consumer for a new firmware incarnation. Live
 * transport recovery must not call this: it would invent ESP_MAX_BUF_CNT
 * free buffers while firmware still holds some of them. */
static int esp_sdio_baseline_tx_credits(struct esp_sdio_context *context)
{
	u32 raw;
	int ret;

	ret = esp_sdio_read_u32(context, ESP_SLAVE_TOKEN_RDATA,
				context->token_buf, &raw, ACQUIRE_LOCK);
	if (ret)
		return ret;

	raw = (raw >> 16) & ESP_TX_BUFFER_MASK;
	if (raw >= ESP_MAX_BUF_CNT)
		context->tx_buffer_count = raw - ESP_MAX_BUF_CNT;
	else
		context->tx_buffer_count = 0;
	sdio_buf_available = 0;
	return 0;
}

static int esp_slave_get_tx_buffer_num(struct esp_sdio_context *context, u32 *tx_num, u8 is_lock_needed)
{
	u32 raw;
	int ret = 0;

	if (!context || !context->token_buf)
		return -ENOMEM;

	ret = esp_sdio_read_u32(context, ESP_SLAVE_TOKEN_RDATA,
				context->token_buf, &raw, is_lock_needed);
	if (ret)
		return ret;

	raw = (raw >> 16) & ESP_TX_BUFFER_MASK;
	raw = (raw + ESP_TX_BUFFER_MAX - context->tx_buffer_count) % ESP_TX_BUFFER_MAX;
	if (raw > ESP_MAX_BUF_CNT)
		raw = ESP_MAX_BUF_CNT;

	*tx_num = raw;

	return ret;
}

int esp_deinit_module(struct esp_adapter *adapter)
{
	/* Second & onward boot-up cleanup is not required for SDIO:
	 * As Removal of SDIO triggers complete Deinit and SDIO insertion/
	 * detection, triggers probing which does initialization.
	 */
	return 0;
}

static u32 esp_sdio_rx_len_delta(u32 raw_len, u32 rx_byte_count)
{
	if (raw_len >= rx_byte_count)
		return (raw_len + ESP_RX_BYTE_MAX - rx_byte_count) % ESP_RX_BYTE_MAX;
	return ESP_RX_BYTE_MAX - rx_byte_count + raw_len;
}

static int esp_get_len_from_slave(struct esp_sdio_context *context, u32 *rx_size, u8 is_lock_needed)
{
	u32 raw_len = 0;
	u32 rx_byte_count;
	u32 delta = 0;
	u32 retry_count = 0;
	int ret = 0;
	bool notified;

	rx_byte_count = context->rx_byte_count;
	notified = atomic_xchg(&context->rx_pending, 0);

	/* Read PACKET_LEN in worker. If notification-driven and delta is zero,
	 * retry to allow slave publication to settle. */
	for (;;) {
		ret = esp_sdio_read_u32(context, ESP_SLAVE_PACKET_LEN_REG,
					context->rx_len_buf, &raw_len, is_lock_needed);
		if (!ret) {
			raw_len &= ESP_SLAVE_LEN_MASK;
			delta = esp_sdio_rx_len_delta(raw_len, rx_byte_count);
		}

		if (!ret && delta <= ESP_HOST_RX_AGGR_SIZE &&
		    (delta || !notified))
			break;

		if (retry_count >= ESP_SDIO_PKT_LEN_ZERO_RETRIES || (!notified && !ret && !delta))
			break;

		retry_count++;
		usleep_range(ESP_SDIO_PKT_LEN_RETRY_MIN_US,
			      ESP_SDIO_PKT_LEN_RETRY_MAX_US);
	}

	if (ret) {
		if (!test_bit(ESP_FW_RECOVERY_PENDING,
			      &context->adapter->state_flags))
			esp_err("ESP_SDIO_RX_ERROR: PACKET_LEN read failed ret=%d "
				"notified=%u retries=%u rx_byte_count=%u\n",
				ret, notified, retry_count, rx_byte_count);
		esp_sdio_request_transport_recovery();
		return ret;
	}

	if (delta > ESP_HOST_RX_AGGR_SIZE) {
		if (!test_bit(ESP_FW_RECOVERY_PENDING,
			      &context->adapter->state_flags))
			esp_err("ESP_SDIO_RX_RESET: PACKET_LEN wrap/reset "
				"raw_len=%u rx_byte_count=%u delta=%u max=%u "
				"retries=%u state=%d; firmware reset observed\n",
				raw_len, rx_byte_count, delta, ESP_HOST_RX_AGGR_SIZE,
				retry_count, atomic_read(&context->adapter->state));
		esp_sdio_request_fw_reset_recovery();
		return -EOVERFLOW;
	}

	*rx_size = delta;
	if (notified && !delta) {
		/* ISR already ACKed NEW_PACKET. If PACKET_LEN has still not
		 * advanced, firmware may not raise another interrupt for this
		 * packet. Re-arm and sample again after releasing the SDIO host. */
		if (test_bit(ESP_TRANSPORT_REMOVING, &context->adapter->state_flags) ||
		    (test_bit(ESP_DRIVER_UNLOADING, &context->adapter->state_flags) &&
		     !test_bit(ESP_ALLOW_DEINIT, &context->adapter->state_flags))) {
			context->rx_len_retry_count = 0;
		} else if (context->rx_len_retry_count < ESP_SDIO_PKT_LEN_DELAYED_RETRIES) {
			context->rx_len_retry_count++;
			atomic_set(&context->rx_pending, 1);
			schedule_delayed_work(&context->rx_len_retry_work,
					      msecs_to_jiffies(1));
		} else {
			context->rx_len_retry_count = 0;
			esp_err("SDIO RX: NEW_PACKET with PACKET_LEN still 0 after settle; requesting recovery\n");
			esp_sdio_request_transport_recovery();
		}
	} else if (delta) {
		context->rx_len_retry_count = 0;
	}
	return 0;
}


#if 0
static void flush_sdio(struct esp_sdio_context *context)
{
	struct sk_buff *skb;

	if (!context || !context->adapter)
		return;

	while (1) {
		skb = read_packet(context->adapter);

		if (!skb) {
			break;
		}

		if (skb->len)
			esp_info("Flushed %d bytes\n", skb->len);
		dev_kfree_skb(skb);
		skb = NULL;
	}
}
#endif

static void esp_remove(struct sdio_func *func)
{
	struct esp_sdio_context *context;
	struct esp_adapter *adapter;
	u32 sdio_clk_mhz;

	if (func->num != 1)
		return;

	context = sdio_get_drvdata(func);
	if (!context)
		return;

	adapter = context->adapter;
	sdio_clk_mhz = context->sdio_clk_mhz;

	if (adapter) {
		/* Publish removal before waiting for events_work. A boot handler that
		 * starts after this point must not reconstruct the card. */
		set_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags);
		set_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
		clear_bit(ESP_INIT_DONE, &adapter->state_flags);
		atomic_set(&adapter->state, ESP_CONTEXT_DISABLED);
		cancel_delayed_work_sync(&adapter->fw_recovery_work);
		if (adapter->events_wq)
			cancel_work_sync(&adapter->events_work);
		skb_queue_purge(&adapter->events_skb_q);
		wake_up(&context->tx_waitq);
	}

#if TEST_RAW_TP
	/* A physical SDIO removal is a transport-session boundary. Stop the
	 * synthetic producer before destroying the queue it targets. */
	if (raw_tp_mode != 0)
		test_raw_tp_cleanup();
#endif

	if (tx_thread && !IS_ERR(tx_thread)) {
		kthread_stop(tx_thread);
		tx_thread = NULL;
	} else if (IS_ERR(tx_thread)) {
		tx_thread = NULL;
	}

	sdio_purge_tx_queues(context);
	skb_queue_purge(&context->rx_q);

	/* Orderly unbind: CLOSE restarts firmware so the next probe gets a
	 * boot TLV. Physical yank fails this write and we continue. */
	if (context->func)
		generate_slave_intr(context, BIT(ESP_CLOSE_DATA_PATH));

	/* Drop the function IRQ before canceling RX work. An ISR that already
	 * passed the state check can otherwise queue if_rx_work after the
	 * first cancel, then we memset the delayed-work item. */
	if (context->func && context->irq_claimed) {
		sdio_claim_host(context->func);
		if (context->irq_claimed) {
			sdio_release_irq(context->func);
			context->irq_claimed = false;
		}
		sdio_release_host(context->func);
	}

	{
		int drain;

		for (drain = 0; drain < 8; drain++) {
			if (adapter && adapter->if_rx_workqueue)
				cancel_work_sync(&adapter->if_rx_work);
			cancel_delayed_work_sync(&context->rx_len_retry_work);
			if ((!adapter || !adapter->if_rx_workqueue ||
			     !work_pending(&adapter->if_rx_work)) &&
			    !delayed_work_pending(&context->rx_len_retry_work))
				break;
		}
	}

	/* The physical function is disappearing: do not issue a final SDIO bus
	 * write and do not notify firmware from ndo_stop(). */
	if (adapter) {
		esp_remove_card(adapter, false);
		adapter->dev = NULL;
	}

	if (context->func)
		deinit_sdio_func(context);

	esp_sdio_free_dma_bufs(context);

	/* sdio_context is a persistent backend singleton. A physical remove must
	 * clear per-binding state without losing the adapter pointer or requested
	 * clock needed by the next probe. */
	memset(context, 0, sizeof(*context));
	context->adapter = adapter;
	context->sdio_clk_mhz = sdio_clk_mhz;
	if (adapter)
		adapter->if_context = context;
	sdio_buf_available = 0;
	atomic_set(&sdio_need_counter_rebase, 0);
	atomic_set(&sdio_need_slave_reset, 0);

	esp_dbg("ESP SDIO cleanup completed\n");
}

static struct sk_buff * esp_sdio_alloc_skb(u32 len)
{
	struct sk_buff *skb = NULL;
	u32 offset;
	u32 alloc_len;

	alloc_len = len + INTERFACE_HEADER_PADDING + SKB_DATA_ADDR_ALIGNMENT;
	skb = netdev_alloc_skb(NULL, alloc_len);

	if (skb) {
		/* Keep the transport headroom and then align the data pointer. */
		skb_reserve(skb, INTERFACE_HEADER_PADDING);
		offset = ((unsigned long)skb->data) & (SKB_DATA_ADDR_ALIGNMENT - 1);
		if (offset)
			skb_reserve(skb, SKB_DATA_ADDR_ALIGNMENT - offset);
	}

	return skb;
}

static void sdio_purge_tx_queues(struct esp_sdio_context *context)
{
	struct sk_buff *skb;
	unsigned long flags;
	int prio;
	u8 cmd_code;
	u16 cmd_seq;

	if (!context)
		return;
	for (prio = 0; prio < MAX_PRIORITY_QUEUES; prio++) {
		spin_lock_irqsave(&context->tx_q[prio].lock, flags);
		while ((skb = __skb_dequeue(&context->tx_q[prio])) != NULL) {
			spin_unlock_irqrestore(&context->tx_q[prio].lock, flags);
			if (context->adapter &&
			    sdio_get_cmd_info(skb, &cmd_code, &cmd_seq))
				esp_cmd_transport_failed(context->adapter,
							 cmd_code, cmd_seq, -EIO);
#if TEST_RAW_TP
			{
				struct esp_payload_header *header = (struct esp_payload_header *)skb->data;
				if (header->if_type == ESP_TEST_IF)
					esp_raw_tp_tx_failed(1);
			}
#endif
			if (atomic_read(&tx_pending))
				atomic_dec(&tx_pending);
			dev_kfree_skb(skb);
			spin_lock_irqsave(&context->tx_q[prio].lock, flags);
		}
		atomic_set(&queue_items[prio], 0);
		spin_unlock_irqrestore(&context->tx_q[prio].lock, flags);
	}
#if TEST_RAW_TP
	if (raw_tp_mode == ESP_TEST_RAW_TP_HOST_TO_ESP)
		esp_raw_tp_queue_resume();
#endif
}

static int esp_sdio_quiesce_for_fw_reset(struct esp_adapter *adapter)
{
	struct esp_sdio_context *context;

	if (!adapter || !adapter->if_context)
		return -EINVAL;

	host_sleep = 0;
	smp_mb();
	context = adapter->if_context;
	atomic_set(&adapter->state, ESP_CONTEXT_DISABLED);

	if (tx_thread && !IS_ERR(tx_thread)) {
		kthread_stop(tx_thread);
		tx_thread = NULL;
	} else if (IS_ERR(tx_thread)) {
		tx_thread = NULL;
	}

	sdio_purge_tx_queues(context);
	skb_queue_purge(&context->rx_q);
	atomic_set(&tx_pending, 0);
	sdio_buf_available = 0;
	cancel_delayed_work_sync(&context->rx_len_retry_work);
	if (adapter->if_rx_workqueue)
		cancel_work_sync(&adapter->if_rx_work);
	return 0;
}

static int esp_sdio_reinit_after_fw_reset(struct esp_adapter *adapter)
{
	struct esp_sdio_context *context;
	int ret;

	if (!adapter || !adapter->if_context)
		return -EINVAL;

	context = adapter->if_context;

	/* Firmware reset also resets its cumulative SDIO packet/credit counters.
	 * Rebase the still-bound host context before accepting the new session. */
	ret = get_firmware_data(context);
	if (ret)
		return ret;

	tx_thread = kthread_run(tx_process, adapter, "esp_TX");
	if (IS_ERR(tx_thread)) {
		ret = PTR_ERR(tx_thread);
		esp_err("Failed to recreate esp_sdio TX thread: %d\n", ret);
		tx_thread = NULL;
		return ret;
	}

	return 0;
}

static void esp_sdio_note_fw_reset(struct esp_adapter *adapter)
{
	if (!adapter)
		return;
	atomic_set(&sdio_need_counter_rebase, 1);
	if (sdio_context.func)
		wake_up(&sdio_context.tx_waitq);
}


static DEFINE_MUTEX(esp_sdio_handshake_mutex);
static atomic_t esp_sdio_hci_write_ambiguous = ATOMIC_INIT(0);

static bool esp_sdio_aggr_has_hci_command(const u8 *data, u16 size)
{
	u32 pos = 0;

	while (data && pos + sizeof(struct esp_payload_header) <= size) {
		const struct esp_payload_header *header =
			(const struct esp_payload_header *)(data + pos);
		u16 len = esp_wire_le16_to_cpu(header->len);
		u16 offset = esp_wire_le16_to_cpu(header->offset);
		u32 frame_len;

		if (!len)
			break;
		if (!ESP_OFFSET_VALID(offset))
			return true;
		frame_len = (u32)offset + len;
		if (frame_len > size - pos)
			return true;
		if (header->if_type == ESP_HCI_IF)
			return true;
		pos += ALIGN(frame_len, 4);
	}
	return false;
}

static int esp_sdio_write_block_guard(struct esp_sdio_context *context, u32 reg,
		u8 *data, u16 size, u8 lock)
{
	bool hci_cmd = esp_sdio_aggr_has_hci_command(data, size);
	int ret = esp_write_block(context, reg, data, size, lock);

	if (ret && hci_cmd)
		atomic_set(&esp_sdio_hci_write_ambiguous, 1);
	return ret;
}

static void esp_sdio_schedule_transport_guard(struct esp_adapter *adapter)
{
	if (atomic_xchg(&esp_sdio_hci_write_ambiguous, 0)) {
		esp_schedule_fw_reset_recovery(adapter);
		esp_request_firmware_restart(adapter);
		return;
	}
	esp_schedule_transport_recovery(adapter);
}

static void esp_sdio_schedule_fw_guard(struct esp_adapter *adapter)
{
	atomic_set(&esp_sdio_hci_write_ambiguous, 0);
	esp_schedule_fw_reset_recovery(adapter);
}

static void esp_sdio_request_transport_recovery(void)
{
	struct esp_adapter *adapter = sdio_context.adapter;

	if (!adapter)
		return;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return;

	if (sdio_context.func)
		wake_up(&sdio_context.tx_waitq);
	esp_sdio_schedule_transport_guard(adapter);
}

static void esp_sdio_request_fw_reset_recovery(void)
{
	struct esp_adapter *adapter = sdio_context.adapter;

	if (!adapter)
		return;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return;

	atomic_set(&sdio_need_counter_rebase, 1);
	if (sdio_context.func)
		wake_up(&sdio_context.tx_waitq);
	esp_sdio_schedule_fw_guard(adapter);
}

static void esp_sdio_request_slave_reset(void)
{
	struct esp_adapter *adapter = sdio_context.adapter;

	if (!adapter)
		return;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return;

	atomic_set(&sdio_need_slave_reset, 1);
	if (sdio_context.func)
		wake_up(&sdio_context.tx_waitq);
	esp_sdio_schedule_transport_guard(adapter);
}

static void esp_sdio_epoch_barrier(struct esp_sdio_context *context)
{
	if (!context || !context->func)
		return;

	/* Own the MMC host before invalidating TX aggregates. An in-flight
	 * CMD53 must finish and commit tx_buffer_count first; bumping the
	 * epoch earlier makes a successful write look unaccounted. */
	sdio_claim_host(context->func);
	atomic_inc(&context->tx_epoch);
	sdio_release_host(context->func);
}

static int esp_sdio_write_scratch8(struct esp_sdio_context *context, u32 reg, u8 val)
{
	return esp_write_reg(context, reg, &val, 1, ACQUIRE_LOCK);
}

static int esp_sdio_wait_slave_reset_done(struct esp_sdio_context *context, u8 gen)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(500);
	u8 done = 0;
	int ret;

	do {
		ret = esp_read_reg(context, ESP_SDIO_RESET_DONE_REG, &done, 1,
				   ACQUIRE_LOCK);
		if (!ret && done == gen)
			return 0;
		msleep(5);
	} while (time_before(jiffies, deadline));

	esp_err("SDIO recover: RESET_DONE timeout gen=%u last=%u ret=%d\n",
		gen, done, ret);
	return -ETIMEDOUT;
}

static int esp_sdio_handshake_slave_reset(struct esp_sdio_context *context, u8 irq_bit)
{
	u8 gen;
	int ret;

	gen = ++sdio_reset_gen;
	if (!gen)
		gen = ++sdio_reset_gen;
	sdio_reset_gen = gen;

	ret = esp_sdio_write_scratch8(context, ESP_SDIO_RESET_DONE_REG, 0);
	if (!ret)
		ret = esp_sdio_write_scratch8(context, ESP_SDIO_RESET_GEN_REG, gen);
	if (!ret)
		ret = generate_slave_intr(context, BIT(irq_bit));
	if (!ret)
		ret = esp_sdio_wait_slave_reset_done(context, gen);
	return ret;
}

static int esp_sdio_reenable_func(struct esp_sdio_context *context)
{
	struct sdio_func *func;
	unsigned int old_timeout;
	int ret;

	func = context->func;
	if (!func)
		return -ENODEV;

	sdio_claim_host(func);
	if (test_bit(ESP_DRIVER_UNLOADING, &context->adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &context->adapter->state_flags)) {
		sdio_release_host(func);
		return -ESHUTDOWN;
	}
	old_timeout = func->enable_timeout;
	if (old_timeout < 5000)
		func->enable_timeout = 5000;
	ret = sdio_enable_func(func);
	if (!ret)
		ret = sdio_set_block_size(func, ESP_BLOCK_SIZE);
	func->enable_timeout = old_timeout;
	if (ret) {
		sdio_release_host(func);
		return ret;
	}

	/* Probe claimed the function IRQ (CCCR IEN) with the host held.
	 * sdio_claim_irq()/sdio_release_irq() do CCCR IENx accesses and
	 * WARN_ON(!host->claimed) in the IRQ get/put path. After
	 * esp_restart() the slave may have dropped those bits; re-arm
	 * them before dropping the host. */
	if (context->irq_claimed) {
		sdio_release_irq(func);
		context->irq_claimed = false;
	}
	ret = sdio_claim_irq(func, esp_handle_isr);
	if (!ret)
		context->irq_claimed = true;
	sdio_release_host(func);
	if (ret)
		esp_err("SDIO recover: re-claim IRQ failed %d\n", ret);
	return ret;
}

static int esp_sdio_rebase_incarnation(struct esp_sdio_context *context)
{
	sdio_purge_tx_queues(context);
	skb_queue_purge(&context->rx_q);
	sdio_buf_available = 0;
	return get_firmware_data(context);
}

static int esp_sdio_open_and_poll(struct esp_adapter *adapter,
		struct esp_sdio_context *context)
{
	int ret;

	ret = generate_slave_intr(context, BIT(ESP_OPEN_DATA_PATH));
	if (ret)
		return ret;

	esp_info("SDIO recover: OPEN_DATA_PATH, waiting for boot event\n");
	msleep(20);
	esp_process_new_packet_intr(adapter);
	return 0;
}

static int esp_sdio_recover_transport_inner(struct esp_adapter *adapter)
{
	struct esp_sdio_context *context;
	u32 raw_len, delta;
	int ret;
	bool fw_reset;

	if (!adapter || !adapter->if_context)
		return -EINVAL;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return -ESHUTDOWN;

	context = adapter->if_context;
	if (!context->func || !context->rx_len_buf || !context->token_buf)
		return -ENODEV;

	host_sleep = 0;
	smp_mb();

	cancel_delayed_work_sync(&context->rx_len_retry_work);

	/* Block new TX commits first. Do not bump the epoch until this
	 * thread owns the MMC host: a CMD53 already in mmc_wait_for_req
	 * must still be allowed to update tx_buffer_count. */
	atomic_set(&adapter->state, ESP_CONTEXT_RX_READY);
	wake_up(&context->tx_waitq);
	esp_sdio_epoch_barrier(context);

	ret = esp_sdio_reenable_func(context);
	if (ret) {
		if (test_bit(ESP_FW_RESET_EXPECTED, &context->adapter->state_flags) ||
		    atomic_read(&sdio_need_counter_rebase))
			atomic_set(&sdio_need_counter_rebase, 1);
		esp_err("SDIO recover: re-enable function failed %d\n", ret);
		return ret;
	}

	fw_reset = test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags);

	if (test_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags)) {
		/* Transport communication has just been restored by esp_sdio_reenable_func.
		 * Deliver the deferred CLOSE_DATA_PATH restart interrupt to firmware now
		 * before waiting for reboot and a new boot TLV. */
		ret = generate_slave_intr(context, BIT(ESP_CLOSE_DATA_PATH));
		if (ret) {
			esp_err("SDIO recover: retry CLOSE_DATA_PATH failed %d; will retry\n", ret);
			return ret;
		}
		clear_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);
		set_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags);
		fw_reset = true;
		msleep(100);
	}

	if (fw_reset)
		atomic_set(&sdio_need_slave_reset, 0);
	else if (atomic_xchg(&sdio_need_slave_reset, 0)) {
		/* Data-phase CMD53 is commit-ambiguous. Reset the SDIO slave
		 * FIFO/PKT_LEN/TOKEN without restarting firmware, then baseline
		 * only after RESET_DONE matches this generation. */
		ret = esp_sdio_handshake_slave_reset(context, ESP_RESET);
		if (ret) {
			esp_err("SDIO recover: ESP_RESET handshake failed %d; escalating\n", ret);
			esp_request_firmware_restart(adapter);
			set_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags);
			atomic_set(&sdio_need_counter_rebase, 1);
			return ret;
		}
		ret = esp_sdio_rebase_incarnation(context);
		if (ret) {
			atomic_set(&sdio_need_slave_reset, 1);
			esp_err("SDIO recover: post-ESP_RESET rebase failed %d\n", ret);
			return ret;
		}
		if (test_bit(ESP_INIT_DONE, &adapter->state_flags)) {
			sdio_buf_available = 0;
			atomic_set(&adapter->state, ESP_CONTEXT_READY);
			esp_process_new_packet_intr(adapter);
			return 1;
		}
		ret = esp_sdio_open_and_poll(adapter, context);
		if (ret)
			return ret;
		return 0;
	}

	if (atomic_xchg(&sdio_need_counter_rebase, 0)) {
		if (!fw_reset) {
			ret = esp_sdio_rebase_incarnation(context);
			if (ret) {
				atomic_set(&sdio_need_counter_rebase, 1);
				esp_err("SDIO recover: rebase counters failed %d\n", ret);
				return ret;
			}
		}
		fw_reset = true;
		set_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags);
	}

	if (fw_reset) {
		ret = esp_sdio_open_and_poll(adapter, context);
		if (ret) {
			/* OPEN may have reached firmware even if the host saw
			 * an error. Rebasing again would skip a published boot
			 * TLV that firmware will not resend. Retry OPEN only. */
			esp_err("SDIO recover: OPEN_DATA_PATH failed %d\n", ret);
			return ret;
		}
		return 0;
	}

	ret = esp_sdio_read_u32(context, ESP_SLAVE_PACKET_LEN_REG,
				context->rx_len_buf, &raw_len, ACQUIRE_LOCK);
	if (ret) {
		esp_err("SDIO recover: PACKET_LEN resync failed %d\n", ret);
		return ret;
	}

	raw_len &= ESP_SLAVE_LEN_MASK;
	delta = esp_sdio_rx_len_delta(raw_len, context->rx_byte_count);

	if (delta > ESP_HOST_RX_AGGR_SIZE) {
		esp_err("SDIO recover: PACKET_LEN wrap during resync "
			"raw_len=%u rx_byte_count=%u delta=%u; treating as firmware reset\n",
			raw_len, context->rx_byte_count, delta);
		set_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags);
		ret = esp_sdio_open_and_poll(adapter, context);
		if (ret) {
			esp_err("SDIO recover: OPEN_DATA_PATH failed %d\n", ret);
			return ret;
		}
		return 0;
	}

	if (test_bit(ESP_INIT_DONE, &adapter->state_flags)) {
		/* OPEN is a probe, not a reincarnation declaration. Live
		 * firmware keeps datapath active; a slave waiting on init_sem
		 * emits a boot TLV that the boot handler will reconstruct. */
		ret = generate_slave_intr(context, BIT(ESP_OPEN_DATA_PATH));
		if (ret) {
			esp_err("SDIO recover: OPEN probe failed %d\n", ret);
			return ret;
		}
		/* Keep the live consumer pointer. Only drop the cached
		 * availability so the next TX re-reads TOKEN. */
		sdio_buf_available = 0;
		atomic_set(&adapter->state, ESP_CONTEXT_READY);
		esp_process_new_packet_intr(adapter);
		return 1;
	}

	/* First boot never completed. Re-OPEN without rebasing. */
	ret = esp_sdio_open_and_poll(adapter, context);
	if (ret)
		return ret;
	return 0;
}

static int esp_sdio_recover_transport(struct esp_adapter *adapter)
{
	int ret;

	/* Own the shared RESET/PS generation from before legacy ++sdio_reset_gen
	 * until recovery has either consumed matching DONE or returned failure. */
	mutex_lock(&esp_sdio_handshake_mutex);
	ret = esp_sdio_recover_transport_inner(adapter);
	mutex_unlock(&esp_sdio_handshake_mutex);
	return ret;
}

static void esp_sdio_flush_bt_traffic(struct esp_adapter *adapter)
{
	struct esp_sdio_context *context = adapter ? adapter->if_context : NULL;
	struct sk_buff *skb, *tmp;
	struct sk_buff_head free_q;
	unsigned long flags;
	int prio;

	if (!context)
		return;

	__skb_queue_head_init(&free_q);
	for (prio = 0; prio < MAX_PRIORITY_QUEUES; prio++) {
		spin_lock_irqsave(&context->tx_q[prio].lock, flags);
		skb_queue_walk_safe(&context->tx_q[prio], skb, tmp) {
			struct esp_payload_header *header;
			if (skb->len < sizeof(*header))
				continue;
			header = (struct esp_payload_header *)skb->data;
			if (header->if_type == ESP_HCI_IF) {
				__skb_unlink(skb, &context->tx_q[prio]);
				if (atomic_read(&tx_pending))
					atomic_dec(&tx_pending);
				atomic_dec(&queue_items[prio]);
				__skb_queue_tail(&free_q, skb);
			}
		}
		spin_unlock_irqrestore(&context->tx_q[prio].lock, flags);
	}

	if (atomic_read(&context->tx_aggr_has_hci)) {
		wait_event_timeout(context->tx_aggr_waitq,
				   atomic_read(&context->tx_aggr_has_hci) == 0,
				   msecs_to_jiffies(100));
		if (atomic_read(&context->tx_aggr_has_hci)) {
			esp_schedule_fw_reset_recovery(adapter);
			esp_request_firmware_restart(adapter);
		}
	}

	spin_lock_irqsave(&context->rx_q.lock, flags);
	skb_queue_walk_safe(&context->rx_q, skb, tmp) {
		struct esp_payload_header *header;
		if (skb->len < sizeof(*header))
			continue;
		header = (struct esp_payload_header *)skb->data;
		if (header->if_type == ESP_HCI_IF) {
			__skb_unlink(skb, &context->rx_q);
			__skb_queue_tail(&free_q, skb);
		}
	}
	spin_unlock_irqrestore(&context->rx_q.lock, flags);

	while ((skb = __skb_dequeue(&free_q)) != NULL)
		dev_kfree_skb(skb);
}

static struct esp_if_ops if_ops = {
	.read		= read_packet,
	.write		= write_packet,
	.alloc_skb	= esp_sdio_alloc_skb,
	.quiesce_for_fw_reset = esp_sdio_quiesce_for_fw_reset,
	.reinit_after_fw_reset = esp_sdio_reinit_after_fw_reset,
	.recover_transport = esp_sdio_recover_transport,
	.note_fw_reset	= esp_sdio_note_fw_reset,
	.flush_bt_traffic = esp_sdio_flush_bt_traffic,
};

static int get_firmware_data(struct esp_sdio_context *context)
{
	u32 val;
	int ret = 0;

	if (!context || !context->rx_len_buf || !context->token_buf)
		return -ENOMEM;

	/* A successful baseline belongs to this incarnation. Do not let a stale
	 * reset-pending flag rebase a live session later. */
	atomic_inc(&context->tx_epoch);
	atomic_set(&sdio_need_counter_rebase, 0);
	atomic_set(&sdio_need_slave_reset, 0);

	ret = esp_sdio_read_u32(context, ESP_SLAVE_PACKET_LEN_REG,
				context->rx_len_buf, &val, ACQUIRE_LOCK);
	if (ret)
		return ret;

	val &= ESP_SLAVE_LEN_MASK;
	esp_info("Rx Pre ====== %d\n", context->rx_byte_count);
	context->rx_byte_count = val;
	esp_info("Rx Pos ======  %d\n", context->rx_byte_count);

	ret = esp_sdio_baseline_tx_credits(context);
	if (ret)
		return ret;
	esp_info("Tx Pos ======  %d\n", context->tx_buffer_count);

	return ret;
}

static int init_context(struct esp_sdio_context *context)
{
	int ret = 0;
	uint8_t prio_q_idx = 0;

	if (!context) {
		return -EINVAL;
	}

	/* Cached host credits belong to one firmware/physical binding only. */
	sdio_buf_available = 0;
	ret = get_firmware_data(context);
	if (ret)
		return ret;

	context->adapter = esp_get_adapter();

	if (unlikely(!context->adapter))
		esp_err("Failed to get adapter\n");

	for (prio_q_idx = 0; prio_q_idx < MAX_PRIORITY_QUEUES; prio_q_idx++) {
		skb_queue_head_init(&(sdio_context.tx_q[prio_q_idx]));
		atomic_set(&queue_items[prio_q_idx], 0);
	}
	skb_queue_head_init(&(sdio_context.rx_q));

	context->adapter->if_type = ESP_IF_TYPE_SDIO;

	return ret;
}

static struct sk_buff *read_packet(struct esp_adapter *adapter)
{
	u32 len_from_slave, padded_len, data_left, payload_left;
	u32 len_to_read, read_len, num_blocks;
	int ret = 0;
	struct sk_buff *skb;
	u8 *pos;
	struct esp_sdio_context *context;
	struct esp_payload_header *header;
	u16 len, offset, frame_len, aligned_len, pos_in_aggr;

	if (!adapter || !adapter->if_context) {
		esp_err("INVALID args\n");
		return NULL;
	}

	context = adapter->if_context;
	skb = skb_dequeue(&(context->rx_q));
	if (skb)
		return skb;

	if (!context || !context->func) {
		esp_err("Invalid context/state\n");
		return NULL;
	}

	sdio_claim_host(context->func);

	data_left = payload_left = len_to_read = read_len = 0;
	len_from_slave = padded_len = num_blocks = 0;

	/* Read length */
	ret = esp_get_len_from_slave(context, &len_from_slave, LOCK_ALREADY_ACQUIRED);

	if (ret) {
		sdio_release_host(context->func);
		return NULL;
	}

	if (!len_from_slave) {
		sdio_release_host(context->func);
		return NULL;
	}

	padded_len = ESP_SDIO_ALIGN_4(len_from_slave);
	skb = esp_if_alloc_skb(context->adapter, padded_len);

	if (!skb) {
		esp_err("SKB alloc failed\n");
		atomic_set(&context->rx_pending, 1);
		schedule_delayed_work(&context->rx_len_retry_work,
				      msecs_to_jiffies(10));
		sdio_release_host(context->func);
		return NULL;
	}

	skb_put(skb, padded_len);
	pos = skb->data;

	data_left = padded_len;
	payload_left = len_from_slave;

	do {
		num_blocks = payload_left/ESP_BLOCK_SIZE;

#if 0
		if (!context->rx_byte_count) {
			start_time = ktime_get_ns();
		}
#endif

		if (num_blocks) {
			len_to_read = num_blocks * ESP_BLOCK_SIZE;
			read_len = len_to_read;
			ret = esp_read_block(context,
					ESP_SLAVE_CMD53_END_ADDR - len_to_read,
					pos, read_len, LOCK_ALREADY_ACQUIRED);
		} else {
			len_to_read = payload_left;
			read_len = ESP_SDIO_ALIGN_4(len_to_read);
			ret = esp_read_block(context,
					ESP_SLAVE_CMD53_END_ADDR - len_to_read,
					pos, read_len, LOCK_ALREADY_ACQUIRED);
		}

		if (ret) {
			if (!test_bit(ESP_FW_RECOVERY_PENDING,
				      &context->adapter->state_flags))
				esp_err("ESP_SDIO_RX_ERROR: data CMD53 read failed ret=%d "
					"blocks=%u payload_len=%u read_len=%u data_left=%u "
					"rx_byte_count=%u state=%d; triggering firmware reset recovery\n",
					ret, num_blocks, len_to_read, read_len, data_left,
					context->rx_byte_count,
					atomic_read(&context->adapter->state));
			esp_schedule_fw_reset_recovery(context->adapter);
			esp_request_firmware_restart(context->adapter);
			dev_kfree_skb(skb);
			skb = NULL;
			sdio_release_host(context->func);
			return NULL;
		}

		data_left -= read_len;
		payload_left -= len_to_read;
		pos += read_len;
		context->rx_byte_count += len_to_read;
		context->rx_byte_count = context->rx_byte_count % ESP_RX_BYTE_MAX;

	} while (data_left > 0);

	sdio_release_host(context->func);
	skb_trim(skb, len_from_slave);

	if (len_from_slave < sizeof(*header)) {
		esp_err("ESP_SDIO_RX_ERROR: runt aggregate len=%u header=%zu\n",
			len_from_slave, sizeof(*header));
		esp_schedule_fw_reset_recovery(context->adapter);
		esp_request_firmware_restart(context->adapter);
		dev_kfree_skb(skb);
		return NULL;
	}

	header = (struct esp_payload_header *)skb->data;
	len = esp_wire_le16_to_cpu(header->len);
	offset = esp_wire_le16_to_cpu(header->offset);

	if (len == 0) {
		esp_err("ESP_SDIO_RX_FRAME_ERROR: zero payload len aggregate=%u "
			"offset=%u if=%u pkt=%u rx_byte_count=%u\n",
			len_from_slave, offset, header->if_type,
			header->packet_type, context->rx_byte_count);
		dev_kfree_skb(skb);
		return NULL;
	}
	if (len > ESP_RX_BUFFER_SIZE || !ESP_OFFSET_VALID(offset)) {
		esp_err("ESP_SDIO_RX_FRAME_ERROR: invalid first frame aggregate=%u "
			"len=%u offset=%u if=%u pkt=%u checksum=0x%04x "
			"rx_byte_count=%u\n",
			len_from_slave, len, offset, header->if_type,
			header->packet_type, esp_wire_le16_to_cpu(header->checksum),
			context->rx_byte_count);
		dev_kfree_skb(skb);
		return NULL;
	}
	frame_len = len + offset;
	if (frame_len > len_from_slave) {
		esp_err("ESP_SDIO_RX_FRAME_ERROR: truncated first frame aggregate=%u "
			"len=%u offset=%u frame=%u if=%u pkt=%u "
			"rx_byte_count=%u\n",
			len_from_slave, len, offset, frame_len, header->if_type,
			header->packet_type, context->rx_byte_count);
		dev_kfree_skb(skb);
		return NULL;
	}
	aligned_len = (frame_len + 3) & ~3;
	if (aligned_len >= len_from_slave) {
		if (frame_len < skb->len)
			skb_trim(skb, frame_len);
		return skb;
	}

	pos_in_aggr = 0;
	while (pos_in_aggr + sizeof(*header) <= len_from_slave) {
		struct sk_buff *frame_skb = NULL;

		header = (struct esp_payload_header *)(skb->data + pos_in_aggr);
		len = esp_wire_le16_to_cpu(header->len);
		offset = esp_wire_le16_to_cpu(header->offset);
		if (!len && !memchr_inv(header, 0, sizeof(*header)))
			break;
		if (!len) {
			esp_err("ESP_SDIO_RX_FRAME_ERROR: zero payload in aggregate "
				"aggregate=%u pos=%u offset=%u if=%u pkt=%u "
				"rx_byte_count=%u\n",
				len_from_slave, pos_in_aggr, offset, header->if_type,
				header->packet_type, context->rx_byte_count);
			break;
		}
		if (len > ESP_RX_BUFFER_SIZE || !ESP_OFFSET_VALID(offset)) {
			esp_err("ESP_SDIO_RX_FRAME_ERROR: invalid aggregate frame "
				"aggregate=%u pos=%u len=%u offset=%u if=%u pkt=%u "
				"checksum=0x%04x rx_byte_count=%u\n",
				len_from_slave, pos_in_aggr, len, offset,
				header->if_type, header->packet_type,
				esp_wire_le16_to_cpu(header->checksum),
				context->rx_byte_count);
			break;
		}
		frame_len = len + offset;
		aligned_len = (frame_len + 3) & ~3;
		if (pos_in_aggr + frame_len > len_from_slave) {
			esp_err("ESP_SDIO_RX_FRAME_ERROR: truncated aggregate frame "
				"aggregate=%u pos=%u len=%u offset=%u frame=%u "
				"if=%u pkt=%u rx_byte_count=%u\n",
				len_from_slave, pos_in_aggr, len, offset, frame_len,
					header->if_type, header->packet_type,
					context->rx_byte_count);
			break;
		}

		frame_skb = esp_if_alloc_skb(adapter, frame_len);
		if (!frame_skb) {
			esp_err("SKB alloc failed for aggregate frame\n");
			break;
		}
		skb_put(frame_skb, frame_len);
		memcpy(frame_skb->data, skb->data + pos_in_aggr, frame_len);
		skb_queue_tail(&(context->rx_q), frame_skb);
		pos_in_aggr += aligned_len;
	}

	dev_kfree_skb(skb);
	return skb_dequeue(&(context->rx_q));
}

static int write_packet(struct esp_adapter *adapter, struct sk_buff *skb)
{
	u32 max_pkt_size = ESP_RX_BUFFER_SIZE - sizeof(struct esp_payload_header);
	struct esp_payload_header *payload_header;
	struct esp_skb_cb *cb = NULL;
	struct command_header *cmd = NULL;
	u16 cmd_seq = 0;
	u8 cmd_code = 0;
	u32 skb_len;
	uint8_t prio = PRIO_Q_LOW;
	bool raw_tp = false;
	bool is_cmd = false;

	if (!adapter || !adapter->if_context || !skb || !skb->data || !skb->len) {
		esp_err("Invalid args\n");
		if (skb)
			dev_kfree_skb(skb);
		return -EINVAL;
	}

	payload_header = (struct esp_payload_header *)skb->data;
	skb_len = skb->len;

	if (atomic_read(&adapter->state) < ESP_CONTEXT_READY ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
	    (test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags))) {
		if (!test_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags) &&
		    !test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags))
			esp_err("Drop TX during shutdown: state=%d flags=0x%lx skb_len=%u if=%u pkt=%u\n",
				atomic_read(&adapter->state), adapter->state_flags,
				skb->len, payload_header->if_type,
				payload_header->packet_type);
		dev_kfree_skb(skb);
		return -ENODEV;
	}

	if (skb->len > max_pkt_size) {
		esp_err("Drop pkt of len[%u] > max SDIO transport len[%u]\n",
			skb->len, max_pkt_size);
		dev_kfree_skb(skb);
		return -EPERM;
	}

#if TEST_RAW_TP
	raw_tp = payload_header->if_type == ESP_TEST_IF;
	if (raw_tp && atomic_read(&tx_pending) >= TX_MAX_PENDING_COUNT) {
		esp_raw_tp_queue_pause();
		if (atomic_read(&tx_pending) < TX_RESUME_THRESHOLD)
			esp_raw_tp_queue_resume();
		H2E_HOST_STATS_INC(h2e_host_drop_queue_full);
		dev_kfree_skb(skb);
		return -EBUSY;
	}
#endif

	cb = (struct esp_skb_cb *)skb->cb;
	if ((payload_header->if_type == ESP_STA_IF ||
	     payload_header->if_type == ESP_AP_IF) &&
	    cb->priv && (atomic_read(&tx_pending) >= TX_MAX_PENDING_COUNT)) {
		esp_tx_pause(cb->priv);
		H2E_HOST_STATS_INC(h2e_host_drop_queue_full);
		dev_kfree_skb(skb);
		return -EBUSY;
	}

	if (payload_header->if_type == ESP_INTERNAL_IF)
		prio = PRIO_Q_HIGH;
	else if (payload_header->if_type == ESP_HCI_IF)
		prio = PRIO_Q_MID;
	else
		prio = PRIO_Q_LOW;

	if (payload_header->packet_type == PACKET_TYPE_COMMAND_REQUEST) {
		u16 offset = esp_wire_le16_to_cpu(payload_header->offset);

		if (offset + sizeof(*cmd) <= skb->len) {
			cmd = (struct command_header *)(skb->data + offset);
			is_cmd = true;
			cmd_code = cmd->cmd_code;
			cmd_seq = esp_wire_le16_to_cpu(cmd->seq_num);
		}
	}

	/* Serialize publication against queue purge and recheck lifecycle after
	 * quiesce has published DISABLED/CLEANUP. If purge already won, this skb
	 * is rejected; if publication won, purge waits for this lock and removes it. */
	spin_lock_bh(&sdio_context.tx_q[prio].lock);
	if (atomic_read(&adapter->state) < ESP_CONTEXT_READY ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
	    (test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags))) {
		spin_unlock_bh(&sdio_context.tx_q[prio].lock);
		dev_kfree_skb(skb);
		return -ESHUTDOWN;
	}

	atomic_inc(&tx_pending);
	__skb_queue_tail(&sdio_context.tx_q[prio], skb);
	atomic_inc(&queue_items[prio]);
	H2E_HOST_STATS_INC(h2e_host_tx_queued);
	spin_unlock_bh(&sdio_context.tx_q[prio].lock);

	/* skb ownership belongs to the TX queue after publication. Use only
	 * values cached before the unlock. */
	if (is_cmd)
		esp_dbg("CMD_SDIO_QUEUE code=%u seq=%u skb_len=%u q=%d/%d/%d pending=%d\n",
			cmd_code, cmd_seq, skb_len,
			atomic_read(&queue_items[PRIO_Q_HIGH]),
			atomic_read(&queue_items[PRIO_Q_MID]),
			atomic_read(&queue_items[PRIO_Q_LOW]),
			atomic_read(&tx_pending));

#if TEST_RAW_TP
	/* Raw-TP has no netdev private pointer. Publish the pause, then recheck
	 * pending so a concurrent consumer resume that happened just before the
	 * pause cannot leave the producer asleep forever. */
	if (raw_tp && atomic_read(&tx_pending) >= TX_MAX_PENDING_COUNT) {
		esp_raw_tp_queue_pause();
		if (atomic_read(&tx_pending) < TX_RESUME_THRESHOLD)
			esp_raw_tp_queue_resume();
	}
#endif

	wake_up(&sdio_context.tx_waitq);
	return 0;
}

static int is_sdio_write_buffer_available(u32 buf_needed,
		u32 *available_before_reserve)
{
#define BUFFER_AVAILABLE        1
#define BUFFER_UNAVAILABLE      0

	int ret = 0;
	struct esp_sdio_context *context = &sdio_context;
	int retry = MAX_WRITE_RETRIES;

	/*If buffer needed are less than buffer available
	  then only read for available buffer number from slave*/
	if (sdio_buf_available < buf_needed) {
		while (retry) {
			ret = esp_slave_get_tx_buffer_num(context, &sdio_buf_available, ACQUIRE_LOCK);
			if (ret) {
				if (available_before_reserve)
					*available_before_reserve = sdio_buf_available;
				return ret;
			}

			if (sdio_buf_available < buf_needed) {
				/* Release SDIO and retry after delay. */
				retry--;
				usleep_range(5, 10);
				continue;
			}

			break;
		}
	}

	if (available_before_reserve)
		*available_before_reserve = sdio_buf_available;

	if (sdio_buf_available >= buf_needed)
		sdio_buf_available -= buf_needed;

	if (!retry) {
		/* No buffer available at slave */
		return BUFFER_UNAVAILABLE;
	}

	return BUFFER_AVAILABLE;
}

static bool sdio_get_cmd_info(struct sk_buff *skb, u8 *cmd_code, u16 *cmd_seq)
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

static bool sdio_cmd_is_current(struct esp_adapter *adapter, u8 cmd_code,
		u16 cmd_seq)
{
	bool is_current_cmd;

	spin_lock_bh(&adapter->cmd_lock);
	is_current_cmd = adapter->cur_cmd && adapter->cur_cmd->cmd_code == cmd_code &&
		adapter->cur_cmd->cmd_seq == cmd_seq &&
		!adapter->cur_cmd->completed;
	spin_unlock_bh(&adapter->cmd_lock);
	return is_current_cmd;
}

static void sdio_fail_abandoned_cmd(struct esp_adapter *adapter, bool has_cmd,
				    u8 cmd_code, u16 cmd_seq, int error,
				    bool has_hci, u32 raw_tp_frames)
{
	if (!adapter)
		return;
#if TEST_RAW_TP
	if (raw_tp_frames)
		esp_raw_tp_tx_failed(raw_tp_frames);
#endif
	if (has_cmd && sdio_cmd_is_current(adapter, cmd_code, cmd_seq))
		esp_cmd_transport_failed(adapter, cmd_code, cmd_seq, error);
	if (has_hci) {
		struct esp_sdio_context *context = adapter->if_context;
		esp_schedule_fw_reset_recovery(adapter);
		esp_request_firmware_restart(adapter);
		if (context) {
			atomic_set(&context->tx_aggr_has_hci, 0);
			wake_up(&context->tx_aggr_waitq);
		}
	}
}

static int tx_process(void *data)
{
	int ret = 0;
	u32 buf_needed = 0;
	u8 *pos = NULL;
	u32 data_left, len_to_send, pad;
	struct sk_buff *tx_skb = NULL;
	struct esp_adapter *adapter = (struct esp_adapter *) data;
	struct esp_sdio_context *context = NULL;
	struct esp_skb_cb *cb = NULL;
	struct esp_payload_header *payload_header = NULL;
	u8 *aggr_buf = NULL;
	u32 aggr_len = 0;
	u32 aggr_frames;
	u32 frame_len = 0;
	u32 credit_available = 0;
	u32 credit_warn_count = 0;
	u32 credit_wait_ms;
	u32 tx_aggr_size;
	u32 raw_tp_frames;
	u32 raw_tp_run_id = 0;
	u32 raw_tp_first_seq;
	u8 aggr_cmd_code = 0;
	u16 aggr_cmd_seq = 0;
	u32 aggr_epoch = 0;
	unsigned long credit_wait_start;
	unsigned long credit_next_warn;
	bool flush_after_pkt = false;
	bool aggr_has_cmd = false;
	bool aggr_has_hci = false;
	bool drop_stale_cmd = false;
	int prio = -1;
	unsigned long qflags;
#ifdef ESP_DEBUG_STATS
	ktime_t aggr_start, credit_start, write_start;
#endif

	context = adapter->if_context;
	if (!context || !context->tx_aggr_buf) {
		esp_err("SDIO TX aggregate buffer missing\n");
		return -ENOMEM;
	}
	aggr_buf = context->tx_aggr_buf;

	while (!kthread_should_stop()) {

		if (atomic_read(&context->adapter->state) < ESP_CONTEXT_READY) {
			msleep(10);
			esp_dbg("not ready\n");
			continue;
		}

		tx_aggr_size = adapter->tx_aggr_size ?
			adapter->tx_aggr_size : ESP_HOST_TX_AGGR_SIZE;
		if (!tx_aggr_size || tx_aggr_size > ESP_TX_AGGR_SIZE_MAX)
			tx_aggr_size = ESP_HOST_TX_AGGR_SIZE;

		if (host_sleep) {
			/* TODO: Use wait_event_interruptible_timeout */
			msleep(100);
			continue;
		}

#ifdef ESP_DEBUG_STATS
		aggr_start = ktime_get();
#endif
		aggr_len = 0;
		aggr_frames = 0;
		raw_tp_frames = 0;
		raw_tp_run_id = 0;
		raw_tp_first_seq = esp_raw_tp_tx_seq_get();
		aggr_has_cmd = false;
		aggr_has_hci = false;
		aggr_cmd_code = 0;
		aggr_cmd_seq = 0;
		drop_stale_cmd = false;
		aggr_epoch = atomic_read(&context->tx_epoch);
		while (aggr_len < tx_aggr_size) {
			prio = -1;
			if (atomic_read(&queue_items[PRIO_Q_HIGH]) > 0)
				prio = PRIO_Q_HIGH;
			else if (atomic_read(&queue_items[PRIO_Q_MID]) > 0)
				prio = PRIO_Q_MID;
			else if (atomic_read(&queue_items[PRIO_Q_LOW]) > 0)
				prio = PRIO_Q_LOW;

			if (prio < 0)
				break;

			/* Take the queue lock for peek, header inspect, and dequeue.
			 * Recovery may sdio_purge_tx_queues() while this kthread is
			 * alive; an unlocked skb_peek() pointer would UAF. */
			spin_lock_irqsave(&context->tx_q[prio].lock, qflags);
			tx_skb = skb_peek(&(context->tx_q[prio]));
			if (!tx_skb) {
				/* Purge can empty the queue after we sampled the
				 * counter. Never extra-dec: that wraps to ~0 and
				 * livelocks wait_event. The counter is owned by
				 * this lock together with enqueue. */
				atomic_set(&queue_items[prio], 0);
				spin_unlock_irqrestore(&context->tx_q[prio].lock, qflags);
				esp_err("SDIO_TX_QUEUE_DESYNC prio=%d counters=%d/%d/%d pending=%d\n",
					prio,
					atomic_read(&queue_items[PRIO_Q_HIGH]),
					atomic_read(&queue_items[PRIO_Q_MID]),
					atomic_read(&queue_items[PRIO_Q_LOW]),
					atomic_read(&tx_pending));
				continue;
			}

			payload_header = (struct esp_payload_header *)tx_skb->data;
			if (!ESP_OFFSET_VALID(esp_wire_le16_to_cpu(payload_header->offset)) ||
			    !esp_wire_le16_to_cpu(payload_header->len)) {
				u16 drop_len = esp_wire_le16_to_cpu(payload_header->len);
				u16 drop_off = esp_wire_le16_to_cpu(payload_header->offset);

				tx_skb = __skb_dequeue(&(context->tx_q[prio]));
				if (tx_skb)
					atomic_dec(&queue_items[prio]);
				spin_unlock_irqrestore(&context->tx_q[prio].lock, qflags);
				esp_err("Drop invalid tx pkt: len=%d offset=%d\n",
					drop_len, drop_off);
				H2E_HOST_STATS_INC(h2e_host_drop_invalid);
				if (tx_skb) {
					if (atomic_read(&tx_pending))
						atomic_dec(&tx_pending);
					dev_kfree_skb(tx_skb);
					tx_skb = NULL;
				}
				continue;
			}
			frame_len = esp_wire_le16_to_cpu(payload_header->offset) +
				esp_wire_le16_to_cpu(payload_header->len);
			if (frame_len > tx_skb->len) {
				u32 drop_frame_len = frame_len;
				u32 drop_skb_len = tx_skb->len;

				tx_skb = __skb_dequeue(&(context->tx_q[prio]));
				if (tx_skb)
					atomic_dec(&queue_items[prio]);
				spin_unlock_irqrestore(&context->tx_q[prio].lock, qflags);
				esp_err("Drop truncated tx pkt: frame_len=%d skb_len=%d\n",
					drop_frame_len, drop_skb_len);
				H2E_HOST_STATS_INC(h2e_host_drop_truncated);
				if (tx_skb) {
					if (atomic_read(&tx_pending))
						atomic_dec(&tx_pending);
					dev_kfree_skb(tx_skb);
					tx_skb = NULL;
				}
				continue;
			}
			len_to_send = (frame_len + 3) & ~3;
			flush_after_pkt = sdio_get_cmd_info(tx_skb, &aggr_cmd_code,
					&aggr_cmd_seq) ||
				(prio == PRIO_Q_LOW &&
				 esp_wire_le16_to_cpu(payload_header->len) <=
				 ESP_HOST_TX_LATENCY_BYPASS_SIZE);
			if (flush_after_pkt && aggr_len) {
				spin_unlock_irqrestore(&context->tx_q[prio].lock, qflags);
				break;
			}
			if (aggr_len + len_to_send > tx_aggr_size) {
				spin_unlock_irqrestore(&context->tx_q[prio].lock, qflags);
				break;
			}

			if (payload_header->if_type == ESP_HCI_IF) {
				aggr_has_hci = true;
				atomic_set(&context->tx_aggr_has_hci, 1);
			}

			tx_skb = __skb_dequeue(&(context->tx_q[prio]));
			if (tx_skb)
				atomic_dec(&queue_items[prio]);
			spin_unlock_irqrestore(&context->tx_q[prio].lock, qflags);
			if (!tx_skb)
				continue;

			if (atomic_read(&tx_pending))
				atomic_dec(&tx_pending);
			if (sdio_get_cmd_info(tx_skb, &aggr_cmd_code, &aggr_cmd_seq)) {
				if (!sdio_cmd_is_current(adapter, aggr_cmd_code,
						aggr_cmd_seq)) {
					esp_err("CMD_SDIO_DROP_STALE_BEFORE_AGGR code=%u seq=%u\n",
						aggr_cmd_code, aggr_cmd_seq);
					dev_kfree_skb(tx_skb);
					tx_skb = NULL;
					continue;
				}
				aggr_has_cmd = true;
			}

			/* Resume ordinary netdev traffic independently from the raw test
			 * producer; raw throughput packets intentionally have no priv. */
			cb = (struct esp_skb_cb *)tx_skb->cb;
			if ((payload_header->if_type == ESP_STA_IF ||
			     payload_header->if_type == ESP_AP_IF) &&
			    cb->priv &&
			    atomic_read(&tx_pending) < TX_RESUME_THRESHOLD)
				esp_tx_resume(cb->priv);
#if TEST_RAW_TP
			if (raw_tp_mode == ESP_TEST_RAW_TP_HOST_TO_ESP &&
			    atomic_read(&tx_pending) < TX_RESUME_THRESHOLD)
				esp_raw_tp_queue_resume();
#endif

			memcpy(aggr_buf + aggr_len, tx_skb->data, frame_len);
#if TEST_RAW_TP
			if (payload_header->if_type == ESP_TEST_IF &&
			    raw_tp_mode == ESP_TEST_RAW_TP_HOST_TO_ESP) {
				struct esp_payload_header *aggr_header =
					(struct esp_payload_header *)(aggr_buf + aggr_len);
				u16 offset = esp_wire_le16_to_cpu(aggr_header->offset);

				if (offset + sizeof(struct raw_tp_packet) <= frame_len) {
					struct raw_tp_packet *tp_pkt =
						(struct raw_tp_packet *)(aggr_buf + aggr_len + offset);
					raw_tp_run_id = esp_wire_le32_to_cpu(tp_pkt->run_id);
				}
				esp_raw_tp_set_seq(aggr_header,
					raw_tp_first_seq + raw_tp_frames);
				if (adapter->capabilities & ESP_CHECKSUM_ENABLED) {
					aggr_header->checksum = 0;
					aggr_header->checksum = esp_wire_cpu_to_le16(compute_checksum(
						aggr_buf + aggr_len, frame_len));
				}
				raw_tp_frames++;
			}
#endif
			if (len_to_send > frame_len)
				memset(aggr_buf + aggr_len + frame_len, 0,
				       len_to_send - frame_len);
			aggr_len += len_to_send;
			aggr_frames++;
			dev_kfree_skb(tx_skb);
			tx_skb = NULL;
			if (flush_after_pkt)
				break;
		}

		if (!aggr_len) {
			/* No TX work pending: block waiting for an enqueue wakeup
			 * instead of polling every 10-20ms. A short timeout acts as
			 * a safety net in case a wakeup is missed. */
			wait_event_interruptible_timeout(context->tx_waitq,
				atomic_read(&queue_items[PRIO_Q_HIGH]) > 0 ||
				atomic_read(&queue_items[PRIO_Q_MID]) > 0 ||
				atomic_read(&queue_items[PRIO_Q_LOW]) > 0 ||
				kthread_should_stop(),
				usecs_to_jiffies(10000));
			continue;
		}
		if (aggr_has_cmd)
			esp_dbg("CMD_SDIO_AGGR_READY code=%u seq=%u len=%u frames=%u\n",
				aggr_cmd_code, aggr_cmd_seq, aggr_len, aggr_frames);
		H2E_HOST_STATS_TIME_ADD(h2e_host_time_aggr_us, aggr_start);

		buf_needed = (aggr_len + tx_aggr_size - 1) / tx_aggr_size;

		/* If SDIO slave buffer is available to write then only write data,
		 * else wait till buffer is available. */
#ifdef ESP_DEBUG_STATS
		credit_start = ktime_get();
#endif
		credit_wait_start = jiffies;
		credit_next_warn = credit_wait_start +
			msecs_to_jiffies(ESP_SDIO_TX_STALL_WARN_MS);
		credit_warn_count = 0;
		do {
			if (aggr_has_cmd && !sdio_cmd_is_current(adapter,
					aggr_cmd_code, aggr_cmd_seq)) {
				drop_stale_cmd = true;
				break;
			}
			if (kthread_should_stop() || host_sleep ||
			    atomic_read(&adapter->state) < ESP_CONTEXT_READY ||
			    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
				break;
			ret = is_sdio_write_buffer_available(buf_needed,
					&credit_available);
			if (ret > 0)
				break;
			if (ret < 0) {
				esp_sdio_request_transport_recovery();
				break;
			}
			H2E_HOST_STATS_INC(h2e_host_no_credit_waits);

			if (time_after_eq(jiffies, credit_next_warn)) {
				credit_warn_count++;
				credit_wait_ms = jiffies_to_msecs(jiffies -
					credit_wait_start);
				credit_next_warn = jiffies +
					msecs_to_jiffies(ESP_SDIO_TX_STALL_WARN_MS);
				esp_err("ESP_SDIO_TX_STALL: waited=%ums reason=%s "
					"token_ret=%d needed=%u available=%u "
					"aggr_len=%u tx_buffer_count=%u tx_pending=%d "
					"queues=%d/%d/%d\n",
					credit_wait_ms,
					ret < 0 ? "token-read-error" : "no-credit",
					ret, buf_needed, credit_available, aggr_len,
					context->tx_buffer_count,
					atomic_read(&tx_pending),
					atomic_read(&queue_items[PRIO_Q_HIGH]),
					atomic_read(&queue_items[PRIO_Q_MID]),
					atomic_read(&queue_items[PRIO_Q_LOW]));
			}
			usleep_range(10, 20);
		} while (!kthread_should_stop());
		H2E_HOST_STATS_TIME_ADD(h2e_host_time_credit_us, credit_start);
		if (kthread_should_stop()) {
			sdio_fail_abandoned_cmd(adapter, aggr_has_cmd,
					aggr_cmd_code, aggr_cmd_seq, -ESHUTDOWN,
					aggr_has_hci, raw_tp_frames);
			break;
		}
		if (drop_stale_cmd) {
			esp_err("CMD_SDIO_DROP_STALE_CREDIT_WAIT code=%u seq=%u waited_ms=%u needed=%u available=%u\n",
				aggr_cmd_code, aggr_cmd_seq,
				jiffies_to_msecs(jiffies - credit_wait_start),
				buf_needed, credit_available);
			sdio_fail_abandoned_cmd(adapter, false, 0, 0, -ETIMEDOUT,
					aggr_has_hci, raw_tp_frames);
			sdio_buf_available = 0;
			continue;
		}
		if (atomic_read(&adapter->state) < ESP_CONTEXT_READY ||
		    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
		    ret < 0) {
			sdio_fail_abandoned_cmd(adapter, aggr_has_cmd,
					aggr_cmd_code, aggr_cmd_seq,
					ret < 0 ? ret : -EIO,
					aggr_has_hci, raw_tp_frames);
			sdio_buf_available = 0;
			continue;
		}
		if (credit_warn_count) {
			credit_wait_ms = jiffies_to_msecs(jiffies -
				credit_wait_start);
			esp_warn("ESP_SDIO_TX_RECOVERED: credits available after "
				"%ums needed=%u available=%u tx_buffer_count=%u\n",
				credit_wait_ms, buf_needed,
				credit_available, context->tx_buffer_count);
		}

		pos = aggr_buf;
		data_left = len_to_send = 0;

		data_left = aggr_len;
		pad = (ESP_BLOCK_SIZE - (data_left % ESP_BLOCK_SIZE)) %
			ESP_BLOCK_SIZE;
		if (pad)
			memset(aggr_buf + aggr_len, 0, pad);
		data_left += pad;

		if (aggr_has_cmd && !sdio_cmd_is_current(adapter,
				aggr_cmd_code, aggr_cmd_seq)) {
			esp_err("CMD_SDIO_DROP_STALE_BEFORE_CMD53 code=%u seq=%u\n",
				aggr_cmd_code, aggr_cmd_seq);
			/* The credit cache was reserved above, but this aggregate is not
			 * written. Force the next TX to refresh authoritative TOKEN state. */
			sdio_fail_abandoned_cmd(adapter, false, 0, 0, -EIO,
					aggr_has_hci, raw_tp_frames);
			sdio_buf_available = 0;
			continue;
		}

#ifdef ESP_DEBUG_STATS
		write_start = ktime_get();
#endif
		if (kthread_should_stop() ||
		    atomic_read(&adapter->state) < ESP_CONTEXT_READY ||
		    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
		    atomic_read(&context->tx_epoch) != aggr_epoch) {
			sdio_fail_abandoned_cmd(adapter, aggr_has_cmd,
					aggr_cmd_code, aggr_cmd_seq,
					kthread_should_stop() ? -ESHUTDOWN : -EIO,
					aggr_has_hci, raw_tp_frames);
			sdio_buf_available = 0;
			continue;
		}

		if (!context->func) {
			sdio_fail_abandoned_cmd(adapter, aggr_has_cmd,
					aggr_cmd_code, aggr_cmd_seq, -ENODEV,
					aggr_has_hci, raw_tp_frames);
			sdio_buf_available = 0;
			continue;
		}

		atomic_set(&tx_in_flight, 1);
		smp_mb();
		sdio_claim_host(context->func);
		if (kthread_should_stop() || host_sleep ||
		    atomic_read(&adapter->state) < ESP_CONTEXT_READY ||
		    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
		    atomic_read(&context->tx_epoch) != aggr_epoch) {
			sdio_release_host(context->func);
			smp_mb();
			atomic_set(&tx_in_flight, 0);
			sdio_fail_abandoned_cmd(adapter, aggr_has_cmd,
					aggr_cmd_code, aggr_cmd_seq, -EAGAIN,
					aggr_has_hci, raw_tp_frames);
			sdio_buf_available = 0;
			continue;
		}
		do {
			len_to_send = data_left;
			ret = esp_sdio_write_block_guard(context, ESP_SLAVE_CMD53_END_ADDR - len_to_send,
					pos, (len_to_send + 3) & (~3), LOCK_ALREADY_ACQUIRED);

			if (ret) {
				esp_err("ESP_SDIO_TX_ERROR: CMD53 write failed ret=%d "
					"write_len=%u data_left=%u aggr_len=%u "
					"buf_needed=%u tx_buffer_count=%u\n",
					ret, len_to_send, data_left, aggr_len,
					buf_needed, context->tx_buffer_count);
				H2E_HOST_STATS_INC(h2e_host_write_fail);
				break;
			}

			data_left -= len_to_send;
			pos += len_to_send;
		} while (data_left);

		if (!ret) {
			context->tx_buffer_count += buf_needed;
			context->tx_buffer_count = context->tx_buffer_count % ESP_TX_BUFFER_MAX;
		}
		sdio_release_host(context->func);
		smp_mb();
		atomic_set(&tx_in_flight, 0);
		H2E_HOST_STATS_TIME_ADD(h2e_host_time_write_us, write_start);

		if (ret) {
			/* drop the packet */
#if TEST_RAW_TP
			if (raw_tp_frames) {
				esp_raw_tp_tx_failed(raw_tp_frames);
				esp_err("RAW_TP_TX_FAILED: first_seq=%u frames=%u "
					"CMD53_ret=%d\n", raw_tp_first_seq,
					raw_tp_frames, ret);
			}
#endif
			if (aggr_has_hci) {
				esp_schedule_fw_reset_recovery(adapter);
				esp_request_firmware_restart(adapter);
				aggr_has_hci = false;
				atomic_set(&context->tx_aggr_has_hci, 0);
				wake_up(&context->tx_aggr_waitq);
			}
			if (aggr_has_cmd) {
				esp_sdio_request_fw_reset_recovery();
				esp_cmd_transport_failed(adapter, aggr_cmd_code,
						aggr_cmd_seq, ret);
				esp_request_firmware_restart(adapter);
			} else {
				esp_sdio_request_slave_reset();
			}
			continue;
		}

		if (aggr_has_hci) {
			aggr_has_hci = false;
			atomic_set(&context->tx_aggr_has_hci, 0);
			wake_up(&context->tx_aggr_waitq);
		}

#if TEST_RAW_TP
		if (raw_tp_frames)
			esp_raw_tp_tx_complete(raw_tp_run_id, raw_tp_frames);
#endif
		if (aggr_has_cmd)
			esp_dbg("CMD_SDIO_CMD53_OK code=%u seq=%u len=%u buffers=%u tx_count=%u\n",
				aggr_cmd_code, aggr_cmd_seq, aggr_len + pad,
				buf_needed, context->tx_buffer_count);
		H2E_HOST_STATS_ADD(h2e_host_tx_sent, aggr_frames);
		print_h2e_host_stats();
	}

	return 0;
}

static struct esp_sdio_context *init_sdio_func(struct sdio_func *func, int *sdio_ret)
{
	struct esp_sdio_context *context = NULL;
	int ret = 0;

	if (!func) {
		return NULL;
	}

	context = &sdio_context;

	context->func = func;

	/* Allocate before sdio_claim_irq(). The ISR and Host→ESP CMD53 path
	 * DMA-map these buffers. */
	context->reg_buf = kmalloc(sizeof(u32), ESP_SDIO_DMA_GFP);
	context->rx_len_buf = kmalloc(sizeof(u32), ESP_SDIO_DMA_GFP);
	context->token_buf = kmalloc(sizeof(u32), ESP_SDIO_DMA_GFP);
	context->tx_aggr_buf = kzalloc(ESP_TX_AGGR_SIZE_MAX, ESP_SDIO_DMA_GFP);
	if (!context->reg_buf || !context->rx_len_buf || !context->token_buf ||
	    !context->tx_aggr_buf) {
		esp_err("Failed to allocate DMA-safe SDIO buffers\n");
		esp_sdio_free_dma_bufs(context);
		context->func = NULL;
		return NULL;
	}
	atomic_set(&context->rx_pending, 0);
	atomic_set(&context->tx_epoch, 0);
	context->rx_len_retry_count = 0;
	context->irq_claimed = false;
	INIT_DELAYED_WORK(&context->rx_len_retry_work, esp_sdio_rx_len_retry_work);
	init_waitqueue_head(&context->tx_waitq);
	atomic_set(&context->tx_aggr_has_hci, 0);
	init_waitqueue_head(&context->tx_aggr_waitq);

	sdio_claim_host(func);

	/* Enable Function */
	ret = sdio_enable_func(func);
	if (ret) {
		esp_err("sdio_enable_func ret: %d\n", ret);
		if (sdio_ret)
			*sdio_ret = ret;
		sdio_release_host(func);
		cancel_delayed_work_sync(&context->rx_len_retry_work);
		esp_sdio_free_dma_bufs(context);
		context->func = NULL;
		return NULL;
	}

	ret = sdio_set_block_size(func, ESP_BLOCK_SIZE);
	if (ret) {
		esp_err("sdio_set_block_size ret: %d\n", ret);
		sdio_disable_func(func);
		if (sdio_ret)
			*sdio_ret = ret;
		sdio_release_host(func);
		cancel_delayed_work_sync(&context->rx_len_retry_work);
		esp_sdio_free_dma_bufs(context);
		context->func = NULL;
		return NULL;
	}

	/* Register IRQ */
	ret = sdio_claim_irq(func, esp_handle_isr);
	if (ret) {
		esp_err("sdio_claim_irq ret: %d\n", ret);
		sdio_disable_func(func);

		if (sdio_ret)
			*sdio_ret = ret;
		sdio_release_host(func);
		cancel_delayed_work_sync(&context->rx_len_retry_work);
		esp_sdio_free_dma_bufs(context);
		context->func = NULL;
		return NULL;
	}
	context->irq_claimed = true;

	/* Set private data */
	sdio_set_drvdata(func, context);

	sdio_release_host(func);

	return context;
}

static int esp_probe(struct sdio_func *func,
				  const struct sdio_device_id *id)
{
	struct esp_sdio_context *context = NULL;
	int ret = 0;

	if (func->num != 1)
		return -ENODEV;

	esp_info("ESP network device detected\n");

	context = init_sdio_func(func, &ret);;
	atomic_set(&tx_pending, 0);

	if (!context) {
		if (ret)
			return ret;
		else
			return -EINVAL;
	}

	if (sdio_context.sdio_clk_mhz) {
		struct mmc_host *host = func->card->host;
		u32 hz = sdio_context.sdio_clk_mhz * NUMBER_1M;
		/* Expansion of mmc_set_clock that isn't exported */
		if (hz < host->f_min)
			hz = host->f_min;
		if (hz > host->f_max)
			hz = host->f_max;
		sdio_claim_host(func);
		host->ios.clock = hz;
		host->ops->set_ios(host, &host->ios);
		sdio_release_host(func);
	}

	ret = init_context(context);
	if (ret) {
		cancel_delayed_work_sync(&context->rx_len_retry_work);
		deinit_sdio_func(context);
		esp_sdio_free_dma_bufs(context);
		context->func = NULL;
		return ret;
	}

	tx_thread = kthread_run(tx_process, context->adapter, "esp_TX");

	if (IS_ERR(tx_thread)) {
		ret = PTR_ERR(tx_thread);
		esp_err("Failed to create esp_sdio TX thread: %d\n", ret);
		tx_thread = NULL;
		cancel_delayed_work_sync(&context->rx_len_retry_work);
		deinit_sdio_func(context);
		esp_sdio_free_dma_bufs(context);
		context->func = NULL;
		return ret;
	}

	context->adapter->dev = &func->dev;
	clear_bit(ESP_TRANSPORT_REMOVING, &context->adapter->state_flags);
	atomic_set(&context->adapter->state, ESP_CONTEXT_RX_READY);
	ret = generate_slave_intr(context, BIT(ESP_OPEN_DATA_PATH));
	if (ret) {
		esp_err("Failed to open data path on slave: %d\n", ret);
		kthread_stop(tx_thread);
		tx_thread = NULL;
		cancel_delayed_work_sync(&context->rx_len_retry_work);
		deinit_sdio_func(context);
		esp_sdio_free_dma_bufs(context);
		context->func = NULL;
		return ret;
	}

	esp_schedule_recovery(context->adapter, ESP_FW_RECOVERY_WATCHDOG_MS, false, false);
	esp_dbg("ESP SDIO probe completed\n");

	return ret;
}

static int esp_suspend_inner(struct device *dev)
{
	struct sdio_func *func = NULL;
	struct esp_sdio_context *context = NULL;
	int ret;

	if (!dev) {
		esp_info("Failed to inform ESP that host is suspending\n");
		return -1;
	}

	func = dev_to_sdio_func(dev);

	esp_info("----> Host Suspend\n");

	context = sdio_get_drvdata(func);

	if (!context) {
		esp_info("Failed to inform ESP that host is suspending\n");
		return -1;
	}

	host_sleep = 1;
	smp_mb();
	{
		int wait_iter = 0;
		while (atomic_read(&tx_in_flight) && wait_iter < 100) {
			usleep_range(1000, 2000);
			wait_iter++;
		}
		if (atomic_read(&tx_in_flight))
			esp_warn("SDIO suspend: tx_in_flight active after wait\n");
	}
	msleep(1000);

	ret = esp_sdio_handshake_slave_reset(context, ESP_POWER_SAVE_ON);
	if (ret) {
		esp_err("SDIO suspend: PS_ON handshake failed %d; quarantining and recovering\n", ret);
		host_sleep = 0;
		smp_mb();
		esp_schedule_fw_reset_recovery(context->adapter);
		esp_request_firmware_restart(context->adapter);
		return ret;
	}

	ret = sdio_set_host_pm_flags(func, MMC_PM_KEEP_POWER);
	if (ret) {
		esp_err("SDIO suspend: MMC_PM_KEEP_POWER failed %d; unwinding\n", ret);
		host_sleep = 0;
		smp_mb();
		if (esp_sdio_handshake_slave_reset(context, ESP_POWER_SAVE_OFF)) {
			esp_schedule_fw_reset_recovery(context->adapter);
			esp_request_firmware_restart(context->adapter);
		}
		return ret;
	}
#if 0
	/* Enale OOB IRQ and host wake up */
	enable_irq(SDIO_OOB_IRQ);
	enable_irq_wake(SDIO_OOB_IRQ);
#endif
	return 0;
}

static int esp_resume_inner(struct device *dev)
{
	struct sdio_func *func = NULL;
	struct esp_sdio_context *context = NULL;
	int ret;

	if (!dev) {
		esp_info("Failed to inform ESP that host is awake\n");
		return -1;
	}

	func = dev_to_sdio_func(dev);

	esp_info("-----> Host Awake\n");
#if 0
	/* Host woke up.. Disable OOB IRQ */
	disable_irq_wake(SDIO_OOB_IRQ);
	disable_irq(SDIO_OOB_IRQ);
#endif


	context = sdio_get_drvdata(func);

	if (!context) {
		esp_info("Failed to inform ESP that host is awake\n");
		return -1;
	}

	ret = esp_sdio_handshake_slave_reset(context, ESP_POWER_SAVE_OFF);
	if (ret) {
		esp_err("SDIO resume: PS_OFF handshake failed %d; quarantining and recovering\n", ret);
		host_sleep = 0;
		smp_mb();
		esp_schedule_fw_reset_recovery(context->adapter);
		esp_request_firmware_restart(context->adapter);
		return ret;
	}

	sdio_buf_available = 0;
	ret = get_firmware_data(context);
	if (ret) {
		esp_err("SDIO resume: counter baseline failed %d; quarantining and recovering\n", ret);
		host_sleep = 0;
		smp_mb();
		esp_schedule_fw_reset_recovery(context->adapter);
		esp_request_firmware_restart(context->adapter);
		return ret;
	}
	host_sleep = 0;
	smp_mb();
	return 0;
}


static int esp_suspend(struct device *dev)
{
	int ret;

	/* Suspend and recovery cannot allocate/overwrite the same generation. */
	mutex_lock(&esp_sdio_handshake_mutex);
	ret = esp_suspend_inner(dev);
	mutex_unlock(&esp_sdio_handshake_mutex);
	return ret;
}

static int esp_resume(struct device *dev)
{
	int ret;

	/* PS_OFF owns the same scratch transaction through DONE and counter
	 * rebasing, preventing a recovery reset from stealing its completion. */
	mutex_lock(&esp_sdio_handshake_mutex);
	ret = esp_resume_inner(dev);
	mutex_unlock(&esp_sdio_handshake_mutex);
	return ret;
}

static const struct dev_pm_ops esp_pm_ops = {
	.suspend = esp_suspend,
	.resume = esp_resume,
};

static const struct of_device_id esp_sdio_of_match[] = {
	{ .compatible = "espressif,esp_sdio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, esp_sdio_of_match);

/* SDIO driver structure to be registered with kernel */
static struct sdio_driver esp_sdio_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= esp_devices,
	.probe		= esp_probe,
	.remove		= esp_remove,
	.drv = {
		.name = KBUILD_MODNAME,
		.owner = THIS_MODULE,
		.pm = &esp_pm_ops,
		.of_match_table = esp_sdio_of_match,
	},
};

int esp_init_interface_layer(struct esp_adapter *adapter, u32 speed)
{
	if (!adapter)
		return -EINVAL;

	adapter->if_context = &sdio_context;
	adapter->if_ops = &if_ops;
	sdio_context.adapter = adapter;
	sdio_context.sdio_clk_mhz = speed;
	sdio_buf_available = 0;

	return sdio_register_driver(&esp_sdio_driver);
}

int esp_validate_chipset(struct esp_adapter *adapter, u8 chipset)
{
	int ret = -1;

	switch(chipset) {
	case ESP_FIRMWARE_CHIP_ESP32:
	case ESP_FIRMWARE_CHIP_ESP32C6:
	case ESP_FIRMWARE_CHIP_ESP32C61:
	case ESP_FIRMWARE_CHIP_ESP32C5:
		adapter->chipset = chipset;
		esp_info("Chipset=%s ID=%02x detected over SDIO\n", esp_chipname_from_id(chipset), chipset);
		ret = 0;
		break;
	case ESP_FIRMWARE_CHIP_ESP32S2:
	case ESP_FIRMWARE_CHIP_ESP32S3:
	case ESP_FIRMWARE_CHIP_ESP32C2:
	case ESP_FIRMWARE_CHIP_ESP32C3:
		esp_err("Chipset=%s ID=%02x not supported for SDIO\n", esp_chipname_from_id(chipset), chipset);
		adapter->chipset = ESP_FIRMWARE_CHIP_UNRECOGNIZED;
		break;
	default:
		esp_err("Unrecognized Chipset ID=%02x\n", chipset);
		adapter->chipset = ESP_FIRMWARE_CHIP_UNRECOGNIZED;
		break;
	}

	return ret;
}

int esp_adjust_spi_clock(struct esp_adapter *adapter, u8 spi_clk_mhz)
{
	/* SPI bus specific call, silently discard */
	return 0;
}

void esp_deinit_interface_layer(void)
{
	sdio_unregister_driver(&esp_sdio_driver);
}
