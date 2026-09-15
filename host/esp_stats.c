// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 */

#include "utils.h"
#include "esp_stats.h"
#include "esp_kernel_port.h"
#include "esp_if.h"

#if TEST_RAW_TP

#include "esp_api.h"
#include <linux/timer.h>
#include <linux/kthread.h>
#include <linux/version.h>

/* Linux 6.15 removed the legacy del_timer_sync() spelling. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
#define esp_del_timer_sync timer_delete_sync
#else
#define esp_del_timer_sync del_timer_sync
#endif

static struct task_struct *raw_tp_tx_thread;
static int test_raw_tp;
static int test_raw_tp__host_to_esp;
static struct timer_list log_raw_tp_stats_timer;
static u8 log_raw_tp_stats_timer_running;
static atomic_long_t test_raw_tp_len = ATOMIC_LONG_INIT(0);
static u32 raw_tp_timer_count;
static u32 raw_tp_tx_seq;
static u32 raw_tp_tx_seq_local;
static u32 raw_tp_rx_expected_seq;
static u64 raw_tp_rx_window;
static u32 host_raw_tp_run_id;
static unsigned long raw_tp_rx_total;
static unsigned long raw_tp_rx_lost;
static unsigned long raw_tp_rx_dup;
static unsigned long raw_tp_rx_reorder;
static u32 raw_tp_rx_error_logs;
static unsigned long raw_tp_tx_failed_frames;
static unsigned long raw_tp_tx_enqueue_failed;
static u8 traffic_open_init_done;
static struct completion traffic_open;
static atomic_t raw_tp_queue_paused = ATOMIC_INIT(0);

static void raw_tp_reset_stats(void)
{
	atomic_long_set(&test_raw_tp_len, 0);
	raw_tp_timer_count = 0;
	raw_tp_tx_seq = 0;
	raw_tp_tx_seq_local = 0;
	raw_tp_rx_expected_seq = 0;
	raw_tp_rx_window = 0;
	raw_tp_rx_total = 0;
	raw_tp_rx_lost = 0;
	raw_tp_rx_dup = 0;
	raw_tp_rx_reorder = 0;
	raw_tp_rx_error_logs = 0;
	raw_tp_tx_failed_frames = 0;
	raw_tp_tx_enqueue_failed = 0;
}

void esp_raw_tp_set_seq(struct esp_payload_header *header, u32 seq)
{
	if (!header)
		return;

	header->flags = seq & 0xff;
	header->reserved1 = (seq >> 8) & 0xff;
	header->reserved2 = (seq >> 16) & 0xff;
	header->reserved3 = (seq >> 24) & 0xff;
}

u32 esp_raw_tp_tx_seq_get(void)
{
	return raw_tp_tx_seq;
}

void esp_raw_tp_tx_failed(u32 frame_count)
{
	raw_tp_tx_failed_frames += frame_count;
}

u32 esp_raw_tp_alloc_run_id(void)
{
	host_raw_tp_run_id++;
	if (!host_raw_tp_run_id)
		host_raw_tp_run_id = 1;
	return host_raw_tp_run_id;
}

u32 esp_raw_tp_get_run_id(void)
{
	if (!host_raw_tp_run_id)
		host_raw_tp_run_id = 1;
	return host_raw_tp_run_id;
}

static u32 raw_tp_get_seq(const struct esp_payload_header *header)
{
	if (!header)
		return 0;

	return (u32)header->flags |
		((u32)header->reserved1 << 8) |
		((u32)header->reserved2 << 16) |
		((u32)header->reserved3 << 24);
}

void esp_raw_tp_tx_complete(u32 run_id, u32 frame_count)
{
	if (run_id && run_id != host_raw_tp_run_id)
		return;
	raw_tp_tx_seq += frame_count;
	atomic_long_add((long)frame_count * TEST_RAW_TP__BUF_SIZE, &test_raw_tp_len);
}

static void log_raw_tp_stats_timer_cb(struct timer_list *timer)
{
	unsigned long actual_bandwidth = 0;
	long bytes_completed = 0;

	if (!READ_ONCE(log_raw_tp_stats_timer_running))
		return;

	mod_timer(&log_raw_tp_stats_timer, jiffies + msecs_to_jiffies(1000));
	bytes_completed = atomic_long_xchg(&test_raw_tp_len, 0);
	if (bytes_completed > 0)
		actual_bandwidth = ((unsigned long)bytes_completed * 8) / 1024;
	esp_dbg("%u-%u sec %lu kbits/sec tx_completed=%u tx_failed=%lu "
		 "enqueue_failed=%lu seq_total=%lu lost=%lu dup=%lu reorder=%lu\n\r",
			raw_tp_timer_count,
			raw_tp_timer_count + 1, actual_bandwidth,
			raw_tp_tx_seq, raw_tp_tx_failed_frames,
			raw_tp_tx_enqueue_failed,
			raw_tp_rx_total, raw_tp_rx_lost, raw_tp_rx_dup,
			raw_tp_rx_reorder);

	raw_tp_timer_count++;
}

static int raw_tp_tx_process(void *data)
{
	int ret = 0;
	struct sk_buff *tx_skb = NULL;
	struct esp_payload_header *payload_header = NULL;
	struct esp_adapter *adapter = NULL;
	struct esp_skb_cb *cb = NULL;
	u8 pad_len = 0;
	u16 total_len = 0;

	pad_len = sizeof(struct esp_payload_header);
	total_len = TEST_RAW_TP__BUF_SIZE + pad_len;
	pad_len += (SKB_DATA_ADDR_ALIGNMENT -
		(total_len % SKB_DATA_ADDR_ALIGNMENT)) % SKB_DATA_ADDR_ALIGNMENT;
	total_len = TEST_RAW_TP__BUF_SIZE + pad_len;

	msleep(2000);

	while (!kthread_should_stop()) {
		if (atomic_read(&raw_tp_queue_paused)) {
			if (wait_for_completion_interruptible(&traffic_open) &&
			    kthread_should_stop())
				break;
			continue;
		}
		adapter = esp_get_adapter();
		if (!adapter) {
			msleep(10);
			continue;
		}

		/* Synthetic host->ESP traffic does not require an associated WLAN
		 * carrier.  The netdev queue is normally stopped until association,
		 * so gating this test on its state would measure enqueue rejection
		 * rather than the transport. */
		if (test_raw_tp__host_to_esp) {

			tx_skb = esp_if_alloc_skb(adapter, total_len);
			if (!tx_skb) {
				esp_info("%u adapter->if_ops->alloc_skb failed\n", __LINE__);
				msleep(10);
				continue;
			}
			skb_put(tx_skb, total_len);
			memset(tx_skb->data, 0, total_len);
			cb = (struct esp_skb_cb *) tx_skb->cb;
			memset(cb, 0, sizeof(*cb));

			payload_header = (struct esp_payload_header *) tx_skb->data;
			memset(payload_header, 0, pad_len);

			payload_header->if_type = ESP_TEST_IF;
			payload_header->if_num = 0;
			payload_header->len = esp_wire_cpu_to_le16(TEST_RAW_TP__BUF_SIZE);
			payload_header->offset = esp_wire_cpu_to_le16(pad_len);
			payload_header->packet_type = PACKET_TYPE_DATA;
			{
				struct raw_tp_packet *tp_pkt = (struct raw_tp_packet *)(tx_skb->data + pad_len);
				u32 cur_seq = raw_tp_tx_seq_local++;
				tp_pkt->seq = esp_wire_cpu_to_le32(cur_seq);
				tp_pkt->run_id = esp_wire_cpu_to_le32(host_raw_tp_run_id);
				esp_raw_tp_set_seq(payload_header, cur_seq);
			}
			if (adapter->capabilities & ESP_CHECKSUM_ENABLED) {
				payload_header->checksum =
					esp_wire_cpu_to_le16(compute_checksum(tx_skb->data,
								(TEST_RAW_TP__BUF_SIZE + pad_len)));
			}
			if (!adapter->if_ops || !adapter->if_ops->write) {
				dev_kfree_skb_any(tx_skb);
				msleep(10);
				continue;
			}
			ret = esp_send_packet(adapter, tx_skb);
			if (ret) {
				raw_tp_tx_enqueue_failed++;
				/* Do not busy-spin when the transport queue is full or
				 * unavailable.  This test must not starve the host it is
				 * measuring. */
				msleep(1);
			}

		} else {
			if (traffic_open_init_done) {
				reinit_completion(&traffic_open);
				wait_for_completion_interruptible(&traffic_open);
			}
		}
	}
	esp_info("raw tp tx thrd stopped\n");
	return 0;
}

static void process_raw_tp_flags(void)
{
	test_raw_tp_cleanup();

	if (test_raw_tp) {
		if (!traffic_open_init_done) {
			init_completion(&traffic_open);
			traffic_open_init_done = 1;
		}
		atomic_set(&raw_tp_queue_paused, 0);

		timer_setup(&log_raw_tp_stats_timer, log_raw_tp_stats_timer_cb, 0);
		WRITE_ONCE(log_raw_tp_stats_timer_running, 1);
		mod_timer(&log_raw_tp_stats_timer, jiffies + msecs_to_jiffies(1000));

		if (test_raw_tp__host_to_esp) {

			raw_tp_tx_thread = kthread_run(raw_tp_tx_process, NULL, "raw tp thrd");
			if (IS_ERR(raw_tp_tx_thread)) {
				esp_err("Failed to create send traffic thread: %ld\n",
					PTR_ERR(raw_tp_tx_thread));
				raw_tp_tx_thread = NULL;
			}

		}
	}
}


static void start_test_raw_tp(int raw_tp__host_to_esp)
{
	test_raw_tp = 1;
	test_raw_tp__host_to_esp = raw_tp__host_to_esp;
}

static void stop_test_raw_tp(void)
{
	test_raw_tp = 0;
	test_raw_tp__host_to_esp = 0;
}

void esp_raw_tp_queue_pause(void)
{
	if (traffic_open_init_done &&
	    atomic_cmpxchg(&raw_tp_queue_paused, 0, 1) == 0)
		reinit_completion(&traffic_open);
}

void esp_raw_tp_queue_resume(void)
{
	if (traffic_open_init_done &&
	    atomic_cmpxchg(&raw_tp_queue_paused, 1, 0) == 1)
		complete_all(&traffic_open);
}

void test_raw_tp_cleanup(void)
{
	int ret = 0;

	if (log_raw_tp_stats_timer_running) {
		WRITE_ONCE(log_raw_tp_stats_timer_running, 0);
		esp_del_timer_sync(&log_raw_tp_stats_timer);
	}

	esp_raw_tp_queue_resume();

	if (raw_tp_tx_thread) {
		ret = kthread_stop(raw_tp_tx_thread);
		if (ret) {
			msleep(10);
			ret = kthread_stop(raw_tp_tx_thread);
		}
		if (ret)
			esp_err("Kthread stop error\n");

		raw_tp_tx_thread = NULL;
	}
	/* Do not reset counters during teardown. An RX worker that passed the
	 * active check just before the timer was stopped may still finish here;
	 * resetting expected_seq underneath it creates a false huge sequence gap.
	 * process_test_capabilities() resets all counters before the next test. */
}

void update_test_raw_tp_rx_stats(const struct esp_payload_header *header,
				 u16 len)
{
	u32 seq;
	u32 run_id = 0;
	s32 diff;
	u16 offset;
	const struct raw_tp_packet *tp_pkt;

	/* Ignore frames racing with teardown after the stats timer was stopped. */
	if (!READ_ONCE(log_raw_tp_stats_timer_running) ||
	    test_raw_tp__host_to_esp || !header)
		return;

	offset = esp_wire_le16_to_cpu(header->offset);
	if (len >= offset + sizeof(struct raw_tp_packet)) {
		tp_pkt = (const struct raw_tp_packet *)((const u8 *)header + offset);
		seq = esp_wire_le32_to_cpu(tp_pkt->seq);
		run_id = esp_wire_le32_to_cpu(tp_pkt->run_id);
	} else {
		seq = raw_tp_get_seq(header);
		run_id = host_raw_tp_run_id;
	}

	if (run_id != host_raw_tp_run_id)
		return;

	diff = (s32)(seq - raw_tp_rx_expected_seq);
	raw_tp_rx_total++;
	atomic_long_add(len, &test_raw_tp_len);

	if (!diff) {
		raw_tp_rx_expected_seq++;
		while (raw_tp_rx_window & 1ULL) {
			raw_tp_rx_expected_seq++;
			raw_tp_rx_window >>= 1;
		}
		raw_tp_rx_window >>= 1;
	} else if (diff > 0) {
		if (diff > 128) {
			u32 i;
			u32 skip;

			raw_tp_rx_lost++;
			raw_tp_rx_expected_seq++;
			for (i = 0; i < 63; i++) {
				if (!(raw_tp_rx_window & 1ULL))
					raw_tp_rx_lost++;
				raw_tp_rx_expected_seq++;
				raw_tp_rx_window >>= 1;
			}
			raw_tp_rx_window = 0;
			skip = (seq - raw_tp_rx_expected_seq) - 63;
			if (skip > 0) {
				raw_tp_rx_lost += skip;
				raw_tp_rx_expected_seq += skip;
			}
		}
		while ((s32)(seq - raw_tp_rx_expected_seq) >= 64) {
			raw_tp_rx_lost++;
			raw_tp_rx_expected_seq++;
			while (raw_tp_rx_window & 1ULL) {
				raw_tp_rx_expected_seq++;
				raw_tp_rx_window >>= 1;
			}
			raw_tp_rx_window >>= 1;
		}
		diff = (s32)(seq - raw_tp_rx_expected_seq);
		if (!diff) {
			raw_tp_rx_expected_seq++;
			while (raw_tp_rx_window & 1ULL) {
				raw_tp_rx_expected_seq++;
				raw_tp_rx_window >>= 1;
			}
			raw_tp_rx_window >>= 1;
		} else {
			if (raw_tp_rx_window & (1ULL << (diff - 1))) {
				raw_tp_rx_dup++;
			} else {
				raw_tp_rx_window |= (1ULL << (diff - 1));
				raw_tp_rx_reorder++;
			}
		}
	} else {
		raw_tp_rx_dup++;
	}
}
#endif

void process_test_capabilities(u32 raw_tp_mode)
{
#if TEST_RAW_TP
	stop_test_raw_tp();
	if (!host_raw_tp_run_id)
		host_raw_tp_run_id = 1;
	raw_tp_reset_stats();
	if (raw_tp_mode == ESP_TEST_RAW_TP_ESP_TO_HOST) {
		start_test_raw_tp(ESP_TEST_RAW_TP__RX);
		esp_info("start testing of ESP->Host raw throughput\n");
	} else if (raw_tp_mode == ESP_TEST_RAW_TP_HOST_TO_ESP) {
		start_test_raw_tp(ESP_TEST_RAW_TP__TX);
		esp_info("start testing of Host->ESP raw throughput\n");
	}
	process_raw_tp_flags();
#endif
}
