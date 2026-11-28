// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESP-Hosted SPI half-duplex/QIO transport.
 *
 * The wire protocol follows ESP-IDF SPI Slave HD mode through the
 * generic Linux SPI API:
 *
 * Bring-up follows the ESP SPI-HD reference flow in SIO. After the initial
 * boot/capability frame, command remains x1 and bulk data can use x4:
 *
 *   command: 8-bit x1
 *   H2E address: 8-bit x1 (Slave-HD data-only QIO)
 *   E2H address: 8-bit x4 when QIO is selected
 *   dummy: 8 clocks
 *   data: x1 during bring-up, x4 after boot
 *
 * The Linux SPI controller owns controller-specific enhanced-QIO framing and
 * DMA/FIFO constraints. This transport describes the Slave-HD wire phases and
 * negotiates transfer sizes with the slave and generic SPI controller limits.
 */

#include "utils.h"

#include <linux/cache.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
#include <linux/sched.h>
#else
#include <linux/sched/task_stack.h>
#endif
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#include "esp_api.h"
#include "esp_cmd.h"
#include "esp_cfg80211.h"
#include "esp_if.h"
#include "esp_kernel_port.h"
#include "esp_host_wire_bounds.h"
#include "esp_spi_common.h"
#include "esp_spi_hd.h"
#include "esp_spi_hd_wire.h"
#include "esp_stats.h"
#include "esp_utils.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
#define SPIHD_SET_DUMMY(xfer) ((xfer)->dummy_data = 1)
#define SPIHD_IS_DUMMY(xfer) ((xfer)->dummy_data)
#else
/* Older generic controllers clock the explicit zero TX byte as the dummy. */
#define SPIHD_SET_DUMMY(xfer) do { } while (0)
#define SPIHD_IS_DUMMY(xfer) 0
#endif

/*
 * Generic SPI APIs evolved after Linux 4.9. Keep the compatibility
 * differences local to this transport; do not change shared host code.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
#define SPIHD_CONTROLLER(spi) ((spi)->master)
#define SPIHD_CTLR_HALF_DUPLEX SPI_MASTER_HALF_DUPLEX
#define SPIHD_CTLR_NO_TX SPI_MASTER_NO_TX
#define SPIHD_CTLR_NO_RX SPI_MASTER_NO_RX
#else
#define SPIHD_CONTROLLER(spi) ((spi)->controller)
#define SPIHD_CTLR_HALF_DUPLEX SPI_CONTROLLER_HALF_DUPLEX
#define SPIHD_CTLR_NO_TX SPI_CONTROLLER_NO_TX
#define SPIHD_CTLR_NO_RX SPI_CONTROLLER_NO_RX
#endif

/* spi_is_bpw_supported() first appeared in 5.3. Match its 8-bit
 * baseline plus controller-advertised widths on older Linux kernels.
 */
static bool spihd_bpw_supported(struct spi_device *spi, u32 bpw)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
	return spi_is_bpw_supported(spi, bpw);
#else
	u32 mask = SPIHD_CONTROLLER(spi)->bits_per_word_mask;

	return bpw == 8 || (bpw && bpw <= 32 && (mask & SPI_BPW_MASK(bpw)));
#endif
}

static bool spihd_buf_is_on_stack(const void *buf)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
	/* Old object_is_on_stack() takes void * despite only inspecting it. */
	return object_is_on_stack((void *)buf);
#else
	return object_is_on_stack(buf);
#endif
}

/* A zero nanosecond cs_change delay avoids legacy default delays.
 * Linux <5.3 has no field to express a zero-nanosecond delay.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 5, 0)
#define SPIHD_CS_CHANGE_NO_DELAY(xfer) \
	((xfer)->cs_change_delay.unit = SPI_DELAY_UNIT_NSECS)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
#define SPIHD_CS_CHANGE_NO_DELAY(xfer) \
	((xfer)->cs_change_delay_unit = SPI_DELAY_UNIT_NSECS)
#else
#define SPIHD_CS_CHANGE_NO_DELAY(xfer) do { } while (0)
#endif

#define SPIHD_READY_TIMEOUT_MS          5000U
#define SPIHD_INITIAL_BOOT_TIMEOUT_MS   8000U
#define SPIHD_READY_POLL_MS             100U
#define SPIHD_CREDIT_TIMEOUT_MS         200U
#define SPIHD_TX_MAX_PENDING            128
/* Small transfers avoid the setup cost of an enhanced multi-phase message. */
#define SPIHD_BULK_MIN_BYTES             256U

static struct esp_spi_hd_context spi_hd;
static atomic_t tx_pending;
static DEFINE_SPINLOCK(spihd_state_lock);
volatile u8 host_sleep;

/* Bound work in each direction on the ordered SPI worker. */
#define SPIHD_RX_WORK_QUOTA 3U
#define SPIHD_TX_WORK_QUOTA 2U

static u32 spihd_rx_limit = SPI_HD_BUF_SIZE;
static bool spi_hd_driver_registered;
static DEFINE_MUTEX(spihd_bus_lock);
/* Serialize reset pulses and recovery negotiation outside the data worker. */
static DEFINE_MUTEX(spihd_recovery_lock);
static DEFINE_MUTEX(spihd_sio_lock);
#define SPIHD_SIO_HDR_MAX 3

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
static u32 spihd_logged_sclk_hz[2];
static u32 spihd_logged_h2e_qio_hz;
#endif

#define SPIHD_CONTROL_BYTES 64U
#define SPIHD_MAX_PHASES 6U

static size_t spihd_control_alignment(void)
{
	return max_t(size_t, SMP_CACHE_BYTES, esp_transport_dma_alignment());
}

static size_t spihd_control_stride(void)
{
	return ALIGN(SPIHD_CONTROL_BYTES, spihd_control_alignment());
}

static size_t spihd_control_offset(void)
{
	return ALIGN(SPI_HD_TX_BATCH_MAX +
		     SPIHD_SIO_HDR_MAX, spihd_control_alignment());
}

/* Derive the segment limit from slave and controller capacities. */
static u32 spihd_effective_tx_segment(u32 negotiated, size_t max_transfer,
				      size_t max_message)
{
	size_t limit;

	if (max_message < SPIHD_SIO_HDR_MAX)
		return 0;
	limit = min_t(size_t, negotiated, max_transfer);
	limit = min_t(size_t, limit, max_message - SPIHD_SIO_HDR_MAX);
	limit &= ~(size_t)(SPI_HD_TX_SEGMENT_MIN - 1);
	return limit < SPI_HD_TX_SEGMENT_MIN ? 0 : limit;
}

/* The state lock serializes work submission and TX admission with shutdown.
 * The worker never holds it across a sleeping SPI/GPIO operation. */
static bool spihd_rx_enabled(void)
{
	enum spihd_state state = READ_ONCE(spi_hd.state);

	return !READ_ONCE(spi_hd.reset_pending) &&
	       (state == SPIHD_BOOTSTRAP || state == SPIHD_RUNNING);
}

static void spihd_set_state(enum spihd_state state)
{
	unsigned long flags;

	spin_lock_irqsave(&spihd_state_lock, flags);
	/* STOPPED can only be left by a new physical bind. Repeated state
	 * publications are allowed; never revive a removed transport. */
	if (spi_hd.reset_pending && (state == SPIHD_BOOTSTRAP ||
				     state == SPIHD_RUNNING))
		goto unlock;
	if (state == SPIHD_STOPPED || state == spi_hd.state ||
	    (state == SPIHD_INITIALIZING && spi_hd.state == SPIHD_STOPPED) ||
	    (state == SPIHD_BOOTSTRAP && (spi_hd.state == SPIHD_INITIALIZING ||
					 spi_hd.state == SPIHD_RECOVERING)) ||
	    (state == SPIHD_RUNNING && (spi_hd.state == SPIHD_BOOTSTRAP ||
				       spi_hd.state == SPIHD_QUIESCING ||
				       spi_hd.state == SPIHD_RECOVERING)) ||
	    ((state == SPIHD_QUIESCING || state == SPIHD_RECOVERING) &&
	     spi_hd.state != SPIHD_STOPPED))
		WRITE_ONCE(spi_hd.state, state);
unlock:
	spin_unlock_irqrestore(&spihd_state_lock, flags);
}

static void spihd_kick(void)
{
	unsigned long flags;

	spin_lock_irqsave(&spihd_state_lock, flags);
	if (spihd_rx_enabled() && spi_hd.workqueue)
		queue_work(spi_hd.workqueue, &spi_hd.work);
	spin_unlock_irqrestore(&spihd_state_lock, flags);
}

static void spihd_quiesce(enum spihd_state state)
{
	spihd_set_state(state);
	if (spi_hd.workqueue)
		cancel_work_sync(&spi_hd.work);
}

static bool spihd_full_duplex(void)
{
	return !(SPIHD_CONTROLLER(spi_hd.spi)->flags & SPIHD_CTLR_HALF_DUPLEX);
}

/* The Linux SPI API advertises independent TX and RX lane capabilities.
 * A controller may support one quad direction without the other; do not
 * discard its usable fast path or issue a quad transfer it cannot perform.
 */
/*
 * The ESP Slave-HD enhanced receive command uses a four-line address
 * phase as well as four-line data. The Linux SPI RX_QUAD capability alone
 * cannot clock out that TX address. Do not advertise a partial QIO receive
 * path until a separately qualified data-only read command is available.
 * A TX-only quad controller can still use data-only QIO for H2E writes.
 */
static u8 spihd_select_data_lines(u32 mode, bool tx, u8 negotiated_lines)
{
	u32 required = tx ? SPI_TX_QUAD : (SPI_TX_QUAD | SPI_RX_QUAD);

	return (mode & required) != required ?
		1 : negotiated_lines;
}

static bool spihd_phase_lane_supported(u32 mode, u8 tx_nbits, u8 rx_nbits)
{
	if ((tx_nbits == SPI_NBITS_QUAD && !(mode & SPI_TX_QUAD)) ||
	    (rx_nbits == SPI_NBITS_QUAD && !(mode & SPI_RX_QUAD)))
		return false;
	return (tx_nbits == 0 || tx_nbits == SPI_NBITS_SINGLE ||
		tx_nbits == SPI_NBITS_QUAD) &&
	       (rx_nbits == 0 || rx_nbits == SPI_NBITS_SINGLE ||
		rx_nbits == SPI_NBITS_QUAD);
}

static bool spihd_quad_available(struct spi_device *spi)
{
	/* At least H2E needs a quad-capable transmitter. */
	return !!(spi->mode & SPI_TX_QUAD);
}

/* Keep each protocol phase intact: generic transfer splitting cannot recreate
 * the ESP-HD command/address/dummy phases of an enhanced transaction.
 */
static int spihd_sync_transfer(struct spi_device *spi,
			       struct spi_transfer *xfer, unsigned int n)
{
	const void *original_tx[SPIHD_MAX_PHASES] = {};
	void *original_rx[SPIHD_MAX_PHASES] = {};
	size_t total = 0, stride = spihd_control_stride();
	size_t transfer_max = spi_max_transfer_size(spi);
	size_t message_max = spi_max_message_size(spi);
	u8 *control_tx = spi_hd.sio_buf + spihd_control_offset();
	u8 *control_rx = control_tx + SPIHD_MAX_PHASES * stride;
	bool verbose = READ_ONCE(log_level) >= ESP_VERBOSE;
	u64 started = verbose ? ktime_get_ns() : 0;
	unsigned int i;
	int ret;

	if (!n || n > ARRAY_SIZE(original_tx))
		return -EINVAL;
	for (i = 0; i < n; i++) {
		if (xfer[i].len > transfer_max ||
		    xfer[i].len > message_max - total) {
			dev_err_ratelimited(&spi->dev,
				"SPI-HD phase %u len=%u exceeds controller limits transfer=%zu message=%zu total=%zu\n",
				i, xfer[i].len, transfer_max, message_max, total);
			return -EMSGSIZE;
		}
		if (!spihd_bpw_supported(spi, xfer[i].bits_per_word) ||
		    !spihd_phase_lane_supported(spi->mode, xfer[i].tx_nbits,
						 xfer[i].rx_nbits) ||
		    (xfer[i].tx_buf && xfer[i].rx_buf && !spihd_full_duplex())) {
			dev_err_ratelimited(&spi->dev,
				"SPI-HD unsupported phase %u bpw=%u tx=%u rx=%u nbits=%u/%u mode=0x%x controller-flags=0x%x\n",
				i, xfer[i].bits_per_word, !!xfer[i].tx_buf,
				!!xfer[i].rx_buf, xfer[i].tx_nbits,
				xfer[i].rx_nbits, spi->mode,
				SPIHD_CONTROLLER(spi)->flags);
			return -EOPNOTSUPP;
		}
		if (xfer[i].speed_hz < SPIHD_CONTROLLER(spi)->min_speed_hz) {
			dev_err_ratelimited(&spi->dev,
				"SPI-HD phase %u clock=%uHz below controller minimum=%uHz\n",
				i, xfer[i].speed_hz, SPIHD_CONTROLLER(spi)->min_speed_hz);
			return -ERANGE;
		}
		total += xfer[i].len;
		if (xfer[i].tx_buf && spihd_buf_is_on_stack(xfer[i].tx_buf)) {
			if (xfer[i].len > SPIHD_CONTROL_BYTES)
				return -EMSGSIZE;
			original_tx[i] = xfer[i].tx_buf;
		}
		if (xfer[i].rx_buf && spihd_buf_is_on_stack(xfer[i].rx_buf)) {
			if (xfer[i].len > SPIHD_CONTROL_BYTES)
				return -EMSGSIZE;
			original_rx[i] = xfer[i].rx_buf;
		}
	}

	/* Control bytes previously lived on the kernel stack, which is not a
	 * DMA-safe SPI buffer. Reuse the staging allocation, with separate cache
	 * lines for TX and RX, without allocating in the transaction hot path.
	 */
	mutex_lock(&spihd_bus_lock);
	for (i = 0; i < n; i++) {
		if (original_tx[i]) {
			memcpy(control_tx + i * stride, original_tx[i], xfer[i].len);
			xfer[i].tx_buf = control_tx + i * stride;
		}
		if (original_rx[i]) {
			xfer[i].rx_buf = control_rx + i * stride;
		}
		if (verbose)
			esp_verbose("SPI-HD phase=%u len=%u tx=%u rx=%u lanes=%u/%u bpw=%u hz=%u dummy=%u cs_change=%u\n",
				i, xfer[i].len, !!xfer[i].tx_buf, !!xfer[i].rx_buf,
				xfer[i].tx_nbits, xfer[i].rx_nbits,
				xfer[i].bits_per_word, xfer[i].speed_hz,
				SPIHD_IS_DUMMY(&xfer[i]), xfer[i].cs_change);
	}
	ret = spi_sync_transfer(spi, xfer, n);
	for (i = 0; i < n; i++) {
		if (original_tx[i])
			xfer[i].tx_buf = original_tx[i];
		if (original_rx[i]) {
			if (!ret)
				memcpy(original_rx[i], xfer[i].rx_buf, xfer[i].len);
			xfer[i].rx_buf = original_rx[i];
		}
	}
	mutex_unlock(&spihd_bus_lock);
	if (ret)
		dev_err_ratelimited(&spi->dev,
			"SPI-HD bus error=%d phases=%u bytes=%zu lines=%u clock=%uMHz tx_inflight=%u rx_inflight=%u\n",
			ret, n, total, spi_hd.data_lines, spi_hd.spi_clk_mhz,
			spi_hd.tx_inflight, spi_hd.rx_inflight);
	if (verbose)
		esp_verbose("SPI-HD message phases=%u bytes=%zu ret=%d elapsed_us=%llu\n",
			n, total, ret, div_u64(ktime_get_ns() - started, 1000));
	return ret;
}

static u32 spihd_effective_speed_hz(void)
{
	u32 requested_hz = (u32)spi_hd.spi_clk_mhz * NUMBER_1M;

	if (spi_hd.payload_max_hz)
		requested_hz = min(requested_hz, spi_hd.payload_max_hz);

	if (!spi_hd.spi_max_hz)
		return requested_hz;

	return min(requested_hz, spi_hd.spi_max_hz);
}

static u8 spihd_wire_cmd_mode(u8 base, u8 data_lines, u8 addr_lines)
{
	if (data_lines != 4)
		return base;

	return base | (addr_lines == 1 ?
		       SPIHD_QIO_DATA_ONLY_MASK : SPIHD_QIO_MASK);
}

static u8 spihd_data_nbits_lines(u8 lines)
{
	return lines == 4 ? SPI_NBITS_QUAD : SPI_NBITS_SINGLE;
}

static u8 spihd_addr_lines(u8 data_lines, bool h2e)
{
	/* Use Slave-HD data-only QIO for writes: x1 control/address, x4 data. */
	if (data_lines == 4 && h2e)
		return 1;

	return data_lines;
}

static u8 spihd_h2e_lines(void)
{
	return spihd_select_data_lines(spi_hd.spi->mode, true,
				      READ_ONCE(spi_hd.data_lines));
}

static u8 spihd_e2h_lines(void)
{
	return spihd_select_data_lines(spi_hd.spi->mode, false,
				      READ_ONCE(spi_hd.data_lines));
}

static u32 spihd_get_le32(const u8 *buf)
{
	return (u32)buf[0] |
	       ((u32)buf[1] << 8) |
	       ((u32)buf[2] << 16) |
	       ((u32)buf[3] << 24);
}

static void spihd_put_le32(u8 *buf, u32 value)
{
	buf[0] = value;
	buf[1] = value >> 8;
	buf[2] = value >> 16;
	buf[3] = value >> 24;
}

static struct sk_buff *spihd_alloc_skb(u32 len)
{
	return esp_spi_alloc_aligned_skb(len, SPI_HD_BUF_SIZE);
}

static void spihd_purge_queues(void)
{
	struct sk_buff *skb;
	u8 cmd_code;
	u16 cmd_seq;
	u8 q;

	for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
		while ((skb = skb_dequeue(&spi_hd.tx_q[q])) != NULL) {
			if (spi_hd.adapter &&
			    esp_spi_get_cmd_info(skb, &cmd_code, &cmd_seq))
				esp_cmd_transport_failed(spi_hd.adapter,
							 cmd_code, cmd_seq, -EIO);
#if TEST_RAW_TP
			if (skb->len >= sizeof(struct esp_payload_header)) {
				struct esp_payload_header *header =
					(struct esp_payload_header *)skb->data;

				if (header->if_type == ESP_TEST_IF)
					esp_raw_tp_tx_failed(1);
			}
#endif
			if (atomic_read(&tx_pending) > 0)
				atomic_dec(&tx_pending);
			dev_kfree_skb_any(skb);
		}
		skb_queue_purge(&spi_hd.rx_q[q]);
	}
	atomic_set(&tx_pending, 0);
#if TEST_RAW_TP
	if (raw_tp_mode & ESP_TEST_RAW_TP_HOST_TO_ESP)
		esp_raw_tp_queue_resume();
#endif
}

/* A combined message must fit in full, including the completion command. */
static bool spihd_batch_fits(size_t len, u8 lines, size_t overhead,
			    size_t max_transfer, size_t max_message)
{
	return lines == 4 && len > 256 && !(len & 3) &&
	       len <= max_transfer && max_transfer >= SPIHD_SIO_HDR_MAX &&
	       overhead <= max_message && len <= max_message - overhead;
}

static bool spihd_combined_wrdma(size_t len, u8 lines)
{
	return spihd_batch_fits(len, lines, 2 * SPIHD_SIO_HDR_MAX,
				spi_max_transfer_size(spi_hd.spi),
				spi_max_message_size(spi_hd.spi));
}

static bool spihd_rx_ack_required(u32 status, u32 pending)
{
	return !(spi_hd.append_dma && !status && pending &&
		 pending <= spihd_rx_limit);
}

static int spihd_enhanced_xfer_lines(u8 base_cmd, bool has_addr, u8 addr,
				     const void *tx, void *rx, size_t len,
				     u8 lines)
{
	struct spi_transfer xfer[6] = {};
	bool h2e = !!tx;
	u8 addr_lines = spihd_addr_lines(lines, h2e);
	u8 cmd = spihd_wire_cmd_mode(base_cmd, lines, addr_lines);

	u8 dummy = 0;
	u8 end_hdr[SPIHD_SIO_HDR_MAX] = { SPIHD_CMD_WR_END, 0, 0 };
	unsigned int data_index = 0;
	bool wide_data;

	unsigned int n = 0;
	u32 speed;
	u8 data_nbits;
	u8 addr_nbits;
	int ret;

	if (!spi_hd.spi || !len || (!!tx == !!rx))
		return -EINVAL;
	if (READ_ONCE(log_level) >= ESP_VERBOSE)
		esp_verbose("SPI-HD command=0x%02x address=%s0x%02x data=%zu lanes=%u/%u direction=%s\n",
			cmd, has_addr ? "" : "none/", addr, len,
			addr_lines, lines, tx ? "H2E" : "E2H");

	speed = spihd_effective_speed_hz();
	if (lines == 1)
		speed = min_t(u32, speed, spi_hd.control_max_hz);
	data_nbits = spihd_data_nbits_lines(lines);
	addr_nbits = spihd_data_nbits_lines(addr_lines);
	/*
	 * Use 32-bit SPI words for aligned multi-lane payloads if the SPI
	 * controller advertises support. This reduces FIFO/DMA service
	 * pressure without making any assumptions about the controller model.
	 * The Slave-HD command/address/dummy phases remain 8-bit.
	 *
	 * Linux SPI transfers describe words in native-endian memory order;
	 * explicitly pack/unpack the byte stream to retain the same on-wire
	 * order on big- and little-endian hosts. Unsupported widths or tails
	 * use the unmodified 8-bit transfer path.
	 */
	wide_data = lines == 4 && len > SPIHD_BULK_MIN_BYTES && !(len & 3) &&
		    spihd_bpw_supported(spi_hd.spi, 32) &&
		    (!tx || (spi_hd.sio_buf &&
			     len <= SPI_HD_TX_BATCH_MAX));

	/*
	 * In SIO the cmd/addr/dummy bytes are plain x1 bytes on the wire, so one
	 * controller transfer carries them (and write data) instead of three or
	 * four separately programmed ones. The wire bits are identical.
	 */
	if (lines == 1 && addr_lines == 1 && spi_hd.sio_buf &&
	    len <= SPI_HD_BUF_SIZE &&
	    len + (has_addr ? 3 : 2) <= spi_max_transfer_size(spi_hd.spi) &&
	    len + (has_addr ? 3 : 2) <= spi_max_message_size(spi_hd.spi)) {
		u8 hdr[SPIHD_SIO_HDR_MAX] = { cmd, has_addr ? addr : 0, 0 };
		size_t hlen = has_addr ? 3 : 2;

		xfer[0].tx_nbits = SPI_NBITS_SINGLE;
		xfer[0].bits_per_word = 8;
		xfer[0].speed_hz = speed;
		if (rx && len <= sizeof(u32) && spihd_full_duplex()) {
			u8 read_tx[SPIHD_SIO_HDR_MAX + sizeof(u32)] = {};
			u8 read_rx[SPIHD_SIO_HDR_MAX + sizeof(u32)] = {};

			/* Same command/address/dummy/read clocks, one TR-mode
			 * controller transfer. Discard the command-phase MISO.
			 * The command-phase RX bytes are not payload.
			 */
			memcpy(read_tx, hdr, hlen);
			xfer[0].tx_buf = read_tx;
			xfer[0].rx_buf = read_rx;
			xfer[0].rx_nbits = SPI_NBITS_SINGLE;
			xfer[0].len = hlen + len;
			n = 1;
			ret = spihd_sync_transfer(spi_hd.spi, xfer, n);
			if (!ret)
				memcpy(rx, read_rx + hlen, len);
		} else if (tx) {
			mutex_lock(&spihd_sio_lock);
			memcpy(spi_hd.sio_buf, hdr, hlen);
			memcpy(spi_hd.sio_buf + hlen, tx, len);
			xfer[0].tx_buf = spi_hd.sio_buf;
			xfer[0].len = hlen + len;
			n = 1;
			ret = spihd_sync_transfer(spi_hd.spi, xfer, n);
			mutex_unlock(&spihd_sio_lock);
		} else {
			xfer[0].tx_buf = hdr;
			xfer[0].len = hlen;
			xfer[1].rx_buf = rx;
			xfer[1].rx_nbits = SPI_NBITS_SINGLE;
			xfer[1].len = len;
			xfer[1].bits_per_word = 8;
			xfer[1].speed_hz = speed;
			n = 2;
			ret = spihd_sync_transfer(spi_hd.spi, xfer, n);
		}
		data_index = n - 1;
		goto out;
	}

	/*
	 * Describe the Slave-HD dummy phase explicitly. Controllers which cannot
	 * generate enhanced-TX wait cycles must emulate or reject it below the
	 * generic SPI API; this protocol driver must not reshape the address.
	 */

	xfer[n].tx_buf = &cmd;
	xfer[n].len = 1;
	xfer[n].tx_nbits = SPI_NBITS_SINGLE;
	xfer[n].bits_per_word = 8;
	xfer[n].speed_hz = speed;
	n++;

	if (has_addr) {
		xfer[n].tx_buf = &addr;
		xfer[n].len = 1;
		xfer[n].tx_nbits = addr_nbits;
		xfer[n].bits_per_word = 8;
		xfer[n].speed_hz = speed;
		n++;
	}

	xfer[n].tx_buf = &dummy;
	xfer[n].len = 1;
	xfer[n].tx_nbits = SPI_NBITS_SINGLE;
	xfer[n].bits_per_word = 8;
	xfer[n].speed_hz = speed;
	SPIHD_SET_DUMMY(&xfer[n]);
	n++;

	if (tx) {
		xfer[n].tx_buf = tx;
		xfer[n].tx_nbits = data_nbits;
		if (wide_data) {
			const u8 *bytes = tx;
			u32 *words = (u32 *)spi_hd.sio_buf;
			size_t i;

			mutex_lock(&spihd_sio_lock);
			for (i = 0; i < len; i += sizeof(u32))
				words[i / sizeof(u32)] =
					(u32)bytes[i] << 24 |
					(u32)bytes[i + 1] << 16 |
					(u32)bytes[i + 2] << 8 |
					bytes[i + 3];
			xfer[n].tx_buf = words;
		}
	} else {
		xfer[n].rx_buf = rx;
		xfer[n].rx_nbits = data_nbits;
	}
	xfer[n].len = len;
	xfer[n].bits_per_word = wide_data ? 32 : 8;
	xfer[n].speed_hz = speed;
	data_index = n++;
	if (base_cmd == SPIHD_CMD_WRDMA && spihd_combined_wrdma(len, lines)) {
		u32 end_speed = spihd_effective_speed_hz();

		end_speed = min_t(u32, end_speed, spi_hd.control_max_hz);
		xfer[data_index].cs_change = 1;
		/* Zero ns selects no extra delay; zero usecs defaults to 10us. */
		SPIHD_CS_CHANGE_NO_DELAY(&xfer[data_index]);
		xfer[n].tx_buf = end_hdr;
		xfer[n].len = sizeof(end_hdr);
		xfer[n].tx_nbits = SPI_NBITS_SINGLE;
		xfer[n].bits_per_word = 8;
		xfer[n].speed_hz = end_speed;
		n++;
	}

	ret = spihd_sync_transfer(spi_hd.spi, xfer, n);
	if (wide_data && tx)
		mutex_unlock(&spihd_sio_lock);
	if (!ret && wide_data && rx) {
		u8 *bytes = rx;
		size_t i;

		for (i = 0; i < len; i += sizeof(u32)) {
			u32 word;

			memcpy(&word, bytes + i, sizeof(word));
			bytes[i] = word >> 24;
			bytes[i + 1] = word >> 16;
			bytes[i + 2] = word >> 8;
			bytes[i + 3] = word;
		}
	}
out:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
	if (!ret) {
		u32 *last_rate = lines == 4 && tx ? &spihd_logged_h2e_qio_hz :
			&spihd_logged_sclk_hz[lines == 4];

		if (xfer[data_index].effective_speed_hz != *last_rate) {
			*last_rate = xfer[data_index].effective_speed_hz;
			esp_info("SPI-HD controller SCLK: requested=%uHz effective=%uHz lines=%u direction=%s\n",
				 speed, xfer[data_index].effective_speed_hz,
				 lines, tx ? "H2E" : "E2H");
		}
	}
#endif
	return ret;
}

static int spihd_command(u8 cmd)
{
	u8 hdr[SPIHD_SIO_HDR_MAX] = { cmd, 0, 0 };
	struct spi_transfer xfer = {
		.tx_buf = hdr,
		.len = sizeof(hdr),
		.tx_nbits = SPI_NBITS_SINGLE,
		.bits_per_word = 8,
		.speed_hz = min_t(u32, spihd_effective_speed_hz(),
				  spi_hd.control_max_hz),
	};

	return spihd_sync_transfer(spi_hd.spi, &xfer, 1);
}

static int spihd_read_reg_lines(u8 addr, void *buf, size_t len, u8 lines)
{
	return spihd_enhanced_xfer_lines(SPIHD_CMD_RDBUF, true, addr,
					 NULL, buf, len, lines);
}

static int spihd_read_reg_stable_lines(u8 addr, u32 *value, u8 lines)
{
	u8 a[4], b[4];
	int ret;
	int i;

	if (!value)
		return -EINVAL;

	ret = spihd_read_reg_lines(addr, a, sizeof(a), lines);
	if (ret)
		return ret;

	for (i = 0; i < 3; i++) {
		ret = spihd_read_reg_lines(addr, b, sizeof(b), lines);
		if (ret)
			return ret;
		if (!memcmp(a, b, sizeof(a))) {
			*value = spihd_get_le32(b);
			return 0;
		}
		memcpy(a, b, sizeof(a));
	}

	/* Changing/torn cumulative words must not masquerade as a stable
	 * snapshot: the caller may use this value to retire a DMA descriptor.
	 */
	return -EAGAIN;
}

static int spihd_read_tx_len_batched(u32 *value)
{
	struct spi_transfer xfer[2] = {};
	u8 tx[2][SPIHD_SIO_HDR_MAX + sizeof(u32)] = {
		{ SPIHD_CMD_RDBUF, SPIHD_REG_TX_BUF_LEN, 0 },
		{ SPIHD_CMD_RDBUF, SPIHD_REG_TX_BUF_LEN, 0 },
	};
	u8 rx[2][SPIHD_SIO_HDR_MAX + sizeof(u32)] = {};
	u32 speed = spihd_effective_speed_hz();
	int ret, i;

	speed = min_t(u32, speed, spi_hd.control_max_hz);
	for (i = 0; i < ARRAY_SIZE(xfer); i++) {
		xfer[i].tx_buf = tx[i];
		xfer[i].rx_buf = rx[i];
		xfer[i].len = sizeof(tx[i]);
		xfer[i].tx_nbits = SPI_NBITS_SINGLE;
		xfer[i].rx_nbits = SPI_NBITS_SINGLE;
		xfer[i].bits_per_word = 8;
		xfer[i].speed_hz = speed;
	}
	xfer[0].cs_change = 1;
	SPIHD_CS_CHANGE_NO_DELAY(&xfer[0]);
	ret = spihd_sync_transfer(spi_hd.spi, xfer, ARRAY_SIZE(xfer));
	if (ret)
		return ret;
	if (!memcmp(rx[0] + SPIHD_SIO_HDR_MAX,
		    rx[1] + SPIHD_SIO_HDR_MAX, sizeof(u32))) {
		*value = spihd_get_le32(rx[1] + SPIHD_SIO_HDR_MAX);
		return 0;
	}
	/* Preserve the existing stable-read retry on changing counters. */
	return spihd_read_reg_stable_lines(SPIHD_REG_TX_BUF_LEN, value, 1);
}

static bool spihd_use_tx_len_mirror(void)
{
	return spihd_full_duplex() && spi_hd.append_dma &&
	       spi_hd.tx_len_mirror &&
	       spi_max_transfer_size(spi_hd.spi) >= 11 &&
	       spi_max_message_size(spi_hd.spi) >= 11;
}

static int spihd_read_tx_len_mirrored(u32 *value)
{
	u8 tx[SPIHD_SIO_HDR_MAX + 2 * sizeof(u32)] = {
		SPIHD_CMD_RDBUF, SPIHD_REG_TX_LEN_MIRROR, 0,
	};
	u8 rx[sizeof(tx)] = {};
	struct spi_transfer xfer = {};
	u32 speed = spihd_effective_speed_hz();
	int ret;

	speed = min_t(u32, speed, spi_hd.control_max_hz);
	xfer.tx_buf = tx;
	xfer.rx_buf = rx;
	xfer.len = sizeof(tx);
	xfer.tx_nbits = SPI_NBITS_SINGLE;
	xfer.rx_nbits = SPI_NBITS_SINGLE;
	xfer.bits_per_word = 8;
	xfer.speed_hz = speed;
	ret = spihd_sync_transfer(spi_hd.spi, &xfer, 1);
	if (ret)
		return ret;
	/* Firmware publishes the higher-address copy first and tags each update.
	 * A single outstanding descriptor prevents 64 publications in this read.
	 */
	if (memcmp(rx + SPIHD_SIO_HDR_MAX,
		   rx + SPIHD_SIO_HDR_MAX + sizeof(u32), sizeof(u32)))
		return spihd_read_reg_stable_lines(SPIHD_REG_TX_BUF_LEN, value, 1);
	*value = spihd_get_le32(rx + SPIHD_SIO_HDR_MAX);
	pr_info_once("SPI-HD mirrored TX length: one 11-byte SIO frame\n");
	return 0;
}

static int spihd_read_reg_stable(u8 addr, u32 *value)
{
	return spihd_read_reg_stable_lines(addr, value, 1);
}

static int spihd_write_reg_lines(u8 addr, u32 value, u8 lines)
{
	u8 buf[4];

	spihd_put_le32(buf, value);
	return spihd_enhanced_xfer_lines(SPIHD_CMD_WRBUF, true, addr,
					 buf, NULL, sizeof(buf), lines);
}

static int spihd_write_reg(u8 addr, u32 value)
{
	return spihd_write_reg_lines(addr, value, 1);
}

static int spihd_rddma(void *buf, size_t len)
{
	u8 lines = len > SPIHD_BULK_MIN_BYTES ? spihd_e2h_lines() : 1;
	int ret;

	/*
	 * ESP SPI-HD configures an 8-bit address phase for every data command.
	 * The DMA commands use address 0, matching esp-hosted-mcu's master
	 * implementation where .address_bits = 8 and .addr defaults to zero.
	 * Keep that address phase explicit in the generic SPI message; the host
	 * controller is responsible for mapping the requested lane widths.
	 */
	spi_hd.rx_inflight = true;
	ret = spihd_enhanced_xfer_lines(SPIHD_CMD_RDDMA, true, 0,
					 NULL, buf, len, lines);
	/* A failed message may have clocked part of the descriptor. */
	if (ret)
		return ret;

	if (!spi_hd.append_dma)
		ret = spihd_command(SPIHD_CMD_INT0);
	if (!ret)
		spi_hd.rx_inflight = false;
	return ret;
}

static int spihd_wrdma_segment(const void *buf, size_t len, u8 wr_lines)
{
	int ret;

	spi_hd.tx_inflight = true;
	ret = spihd_enhanced_xfer_lines(SPIHD_CMD_WRDMA, true, 0,
					 buf, NULL, len, wr_lines);
	if (ret)
		return ret;
	if (!spihd_combined_wrdma(len, wr_lines))
		ret = spihd_command(SPIHD_CMD_WR_END);
	if (!ret)
		spi_hd.tx_inflight = false;
	return ret;
}

static int spihd_wait_slave_ready(void)
{
	unsigned int waited = 0;
	u32 value = 0;
	u32 last_value = 0;
	int last_ret = -ETIMEDOUT;
	int ret;

	spi_hd.tx_len_mirror = false;
	spi_hd.append_dma = false;
	while (waited < SPIHD_READY_TIMEOUT_MS) {
		ret = spihd_read_reg_stable(SPIHD_REG_SLAVE_READY, &value);
		last_ret = ret;
		if (!ret) {
			last_value = value;
			if ((value & 0xff) == SPIHD_STATE_SLAVE_READY) {
				if (!(value & SPIHD_CAP_PROTOCOL_V3)) {
					esp_err("SPI-HD firmware protocol version unsupported\n");
					return -EPROTONOSUPPORT;
				}
				spi_hd.tx_len_mirror = !!(value & SPIHD_CAP_TX_LEN_MIRROR);
				spi_hd.append_dma = !!(value & SPIHD_CAP_DMA_APPEND);
				return 0;
			}
		}
		msleep(SPIHD_READY_POLL_MS);
		waited += SPIHD_READY_POLL_MS;
	}

	esp_err("SPI-HD slave did not become ready within %u ms: read_ret=%d last=0x%08x\n",
		SPIHD_READY_TIMEOUT_MS, last_ret, last_value);
	return -ETIMEDOUT;
}

static int spihd_read_session(u32 *lo, u32 *hi)
{
	u32 ready;
	int ret;

	ret = spihd_read_reg_stable(SPIHD_REG_SESSION_LO, lo);
	if (ret)
		return ret;
	ret = spihd_read_reg_stable(SPIHD_REG_SESSION_HI, hi);
	if (ret)
		return ret;
	ret = spihd_read_reg_stable(SPIHD_REG_SLAVE_READY, &ready);
	if (ret)
		return ret;
	if ((ready & (0xff | SPIHD_CAP_PROTOCOL_V3)) !=
	    (SPIHD_STATE_SLAVE_READY | SPIHD_CAP_PROTOCOL_V3) || !(*lo | *hi))
		return -EAGAIN;
	return 0;
}

static int spihd_negotiate_limits(void)
{
	struct spi_device *spi = spi_hd.spi;
	u32 tx_max, rx_max, segment, window, lo, hi, check_lo, check_hi;
	u32 control_hz, payload_hz, ctrl, receive_max;
	int ret;

	ret = spihd_read_session(&lo, &hi);
	if (ret)
		return ret;
	ret = spihd_read_reg_stable(SPIHD_REG_SLAVE_CTRL, &ctrl);
	if (ret)
		return ret;
	/* A fresh incarnation has not accepted a host. Never reset cumulative
	 * counters and reopen an already active slave without a real reset. */
	if (ctrl & SPIHD_CTRL_DATAPATH_ON)
		return -EBUSY;
	ret = spihd_read_reg_stable(SPIHD_REG_RX_CREDIT_WINDOW, &window);
	if (ret)
		return ret;
	if (!window || window > SPIHD_MAX_RX_CREDIT_WINDOW)
		return -EPROTO;
	ret = spihd_read_reg_stable(SPIHD_REG_CONTROL_MAX_HZ, &control_hz);
	if (ret)
		return ret;
	ret = spihd_read_reg_stable(SPIHD_REG_PAYLOAD_MAX_HZ, &payload_hz);
	if (ret)
		return ret;
	if (!control_hz || !payload_hz || control_hz > payload_hz ||
	    payload_hz > SPI_HD_MAX_CLK_MHZ * NUMBER_1M)
		return -EPROTO;
	ret = spihd_read_reg_stable(SPIHD_REG_MAX_TX_BUF_LEN, &tx_max);
	if (ret)
		return ret;
	ret = spihd_read_reg_stable(SPIHD_REG_MAX_RX_BUF_LEN, &rx_max);
	if (ret)
		return ret;
	if (tx_max < SPI_HD_BUF_SIZE || tx_max > SPI_HD_TX_BATCH_MAX ||
	    rx_max < SPI_HD_TX_SEGMENT_MIN || rx_max > SPI_HD_TX_BATCH_MAX)
		return -EPROTO;

	receive_max = spihd_effective_tx_segment(tx_max,
			spi_max_transfer_size(spi), spi_max_message_size(spi));
	if (receive_max < SPI_HD_BUF_SIZE)
		return -EMSGSIZE;
	segment = spihd_effective_tx_segment(rx_max,
			spi_max_transfer_size(spi), spi_max_message_size(spi));
	if (!segment)
		return -EMSGSIZE;
	ret = spihd_read_session(&check_lo, &check_hi);
	if (ret)
		return ret;
	if (lo != check_lo || hi != check_hi)
		return -EAGAIN;
	spi_hd.control_max_hz = control_hz;
	spi_hd.payload_max_hz = payload_hz;
	spi_hd.session_lo = lo;
	spi_hd.session_hi = hi;
	spi_hd.rx_credit_window = window;
	spihd_rx_limit = receive_max;
	spi_hd.tx_segment_max = segment;
	esp_info("SPI-HD negotiated: slave-tx=%u host-rx=%u host-segment=%u credits=%u append=%u mirror=%u\n",
		 tx_max, receive_max, segment, window, spi_hd.append_dma, spi_hd.tx_len_mirror);
	return 0;
}

static int spihd_open_slave_datapath(void)
{
	u32 lo, hi, ctrl, capacity;
	int ret, open_ret;

	/* Publish capacity before the open marker. Verify it before admitting
	 * the peer to the datapath, including a write with ambiguous status. */
	ret = spihd_write_reg(SPIHD_REG_HOST_RX_MAX, spihd_rx_limit);
	if (ret)
		return ret;
	ret = spihd_read_reg_stable(SPIHD_REG_HOST_RX_MAX, &capacity);
	if (ret)
		return ret;
	if (capacity != spihd_rx_limit)
		return -EIO;
	open_ret = spihd_write_reg(SPIHD_REG_SLAVE_CTRL, SPIHD_CTRL_DATAPATH_ON);
	ret = spihd_read_reg_stable(SPIHD_REG_SLAVE_CTRL, &ctrl);
	if (ret)
		return ret;
	ret = spihd_read_session(&lo, &hi);
	if (ret)
		return ret;
	if (lo != spi_hd.session_lo || hi != spi_hd.session_hi)
		return -EAGAIN;
	if (!(ctrl & SPIHD_CTRL_DATAPATH_ON))
		return open_ret ?: -EIO;
	return 0;
}

static int spihd_data_ready(void);
static int spihd_receive_one(void);

static bool spihd_tx_allowed(void)
{
	struct esp_adapter *adapter = spi_hd.adapter;

	return READ_ONCE(spi_hd.state) == SPIHD_RUNNING &&
	       !READ_ONCE(spi_hd.reset_pending) && adapter &&
	       atomic_read(&adapter->state) >= ESP_CONTEXT_READY &&
	       !test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) &&
	       !test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags);
}

static u8 spihd_payload_lines(size_t len, u8 available)
{
	return len > SPIHD_BULK_MIN_BYTES ? available : 1;
}

/* The outstanding window is smaller than one byte. On a live session,
 * a torn cumulative read spanning a carry is either a valid older credit
 * or outside the window. Retain all 32 bits to reject stale high-byte
 * aliases after a restart; the recovery path verifies session identity. */
static u32 spihd_credit_delta(u32 published, u32 consumed, u32 window)
{
	u32 delta = published - consumed;

	return delta && delta <= window ? delta : 0;
}

static int spihd_wait_tx_credit(void)
{
	u8 raw[sizeof(u32)];
	u32 published = 0, free_slots = 0;
	unsigned long deadline;
	int ret, rx_ret, ready;

	if (!spihd_tx_allowed())
		return -ESHUTDOWN;
	if (spihd_credit_delta(spi_hd.rx_credit_limit, spi_hd.tx_segment_count,
			       spi_hd.rx_credit_window))
		return 0;
	deadline = jiffies + msecs_to_jiffies(SPIHD_CREDIT_TIMEOUT_MS);
	do {
		if (!spihd_tx_allowed())
			return -ESHUTDOWN;
		ret = spihd_read_reg_lines(SPIHD_REG_RX_BUF_LEN, raw,
					   sizeof(raw), 1);
		if (!ret) {
			published = spihd_get_le32(raw);
			free_slots = spihd_credit_delta(published,
					spi_hd.tx_segment_count, spi_hd.rx_credit_window);
			if (free_slots) {
				spi_hd.rx_credit_limit = spi_hd.tx_segment_count + free_slots;
				return 0;
			}
		}
		if (!spihd_tx_allowed())
			return -ESHUTDOWN;
		/* The same worker owns both directions. Drain at most one RX
		 * descriptor between credit reads so a stalled TX cannot block
		 * the peer's traffic. Keep the original TX recovery deadline. */
		ready = spihd_data_ready();
		if (ready < 0)
			return ready;
		if (ready) {
			rx_ret = spihd_receive_one();
			if (rx_ret && (rx_ret != -EAGAIN || spi_hd.rx_inflight))
				return rx_ret;
		}
		cond_resched();
	} while (time_before(jiffies, deadline));

	esp_err("SPI-HD TX credit timeout: read_ret=%d RX_BUF_LEN=0x%08x tx_segments=%u free=%u\n",
		ret, published, spi_hd.tx_segment_count, free_slots);
	return -ETIMEDOUT;
}

static int spihd_send_frame(struct sk_buff *skb)
{
	const struct esp_payload_header *header;
	const u8 *data;
	size_t wire_len, offset = 0;
	size_t segment = READ_ONCE(spi_hd.tx_segment_max);
	u8 lines;
	int ret;

	if (!skb || !skb->data || skb->len < sizeof(*header))
		return -EINVAL;
	if (skb->len > SPI_HD_TX_BATCH_MAX || !segment)
		return -EMSGSIZE;

	header = (const void *)skb->data;
	lines = header->packet_type != PACKET_TYPE_DATA ? 1 : spihd_h2e_lines();

	/* The shared protocol aligns each frame on the wire; padding is not
	 * included in the Hosted header or checksum. An aligned aggregate
	 * already resides in the staging buffer and needs no copy here. */
	data = skb->data;
	wire_len = SPIHD_WIRE_SPAN(skb->len);
	if (wire_len != skb->len) {
		memcpy(spi_hd.tx_batch_buf, data, skb->len);
		memset(spi_hd.tx_batch_buf + skb->len, 0, wire_len - skb->len);
		data = spi_hd.tx_batch_buf;
	}

	while (offset < wire_len) {
		size_t chunk = min(segment, wire_len - offset);

		ret = spihd_wait_tx_credit();
		if (!ret && !spihd_tx_allowed())
			ret = -ESHUTDOWN;
		if (ret) {
			if (offset)
				spi_hd.tx_inflight = true;
			return ret;
		}
		ret = spihd_wrdma_segment(data + offset, chunk,
					  spihd_payload_lines(chunk, lines));
		if (ret)
			return ret;
		spi_hd.tx_segment_count++;
		offset += chunk;
	}
	return 0;
}

/* Validate all frame boundaries and padding before publishing any frame. */
static int spihd_validate_rx_batch(const u8 *data, size_t total, u32 *frames)
{
	size_t pos = 0;
	u32 count = 0;

	if (!data || !total || !frames)
		return -EINVAL;
	while (pos < total) {
		const struct esp_payload_header *header;
		size_t frame_len, span, i;
		u16 offset, len;

		if (total - pos < sizeof(*header))
			return -EPROTO;
		header = (const struct esp_payload_header *)(data + pos);
		offset = esp_wire_le16_to_cpu(header->offset);
		len = esp_wire_le16_to_cpu(header->len);
		frame_len = (size_t)offset + len;
		if (header->if_type >= ESP_MAX_IF ||
		    !esp_host_wire_span_valid(len, offset, SPI_HD_BUF_SIZE, NULL) ||
		    !esp_host_wire_span_valid(len, offset, total - pos, NULL))
			return -EPROTO;
		span = SPIHD_WIRE_SPAN(frame_len);
		if (span > total - pos)
			return -EPROTO;
		for (i = frame_len; i < span; i++)
			if (data[pos + i])
				return -EPROTO;
		pos += span;
		count++;
	}
	*frames = count;
	return 0;
}

static int spihd_process_rx_buf(struct sk_buff *skb)
{
	struct esp_payload_header *header;
	u16 len;
	u16 offset;

	if (!skb || skb->len < sizeof(*header))
		return -EINVAL;

	header = (struct esp_payload_header *)skb->data;
	if (header->if_type >= ESP_MAX_IF)
		return -EINVAL;

	offset = esp_wire_le16_to_cpu(header->offset);
	len = esp_wire_le16_to_cpu(header->len);
	if (!esp_host_wire_span_valid(len, offset, skb->len, NULL))
		return -EINVAL;

	skb_trim(skb, offset + len);
	if (header->if_type == ESP_INTERNAL_IF)
		skb_queue_tail(&spi_hd.rx_q[PRIO_Q_HIGH], skb);
	else if (header->if_type == ESP_HCI_IF || header->if_type == ESP_RCP_IF)
		skb_queue_tail(&spi_hd.rx_q[PRIO_Q_MID], skb);
	else
		skb_queue_tail(&spi_hd.rx_q[PRIO_Q_LOW], skb);

	esp_process_new_packet_intr(spi_hd.adapter);
	return 0;
}

static int spihd_process_rx_batch(struct sk_buff *skb)
{
	struct sk_buff_head packets;
	struct sk_buff *packet;
	size_t pos = 0;
	u32 frames;
	int ret;

	ret = spihd_validate_rx_batch(skb->data, skb->len, &frames);
	if (ret) {
		esp_err("SPI-HD malformed RX batch: bytes=%u consumed=0x%06x limit=%u append=%u\n",
			skb->len, spi_hd.rx_byte_count, spihd_rx_limit, spi_hd.append_dma);
		if (spi_hd.append_dma)
			spi_hd.rx_inflight = true;
		goto out;
	}
	if (frames == 1) {
		ret = spihd_process_rx_buf(skb);
		if (ret)
			dev_kfree_skb_any(skb);
		return ret;
	}
	/* Allocate every output before any is visible to the receive worker. */
	skb_queue_head_init(&packets);
	while (pos < skb->len) {
		struct esp_payload_header *header = (void *)(skb->data + pos);
		size_t len = esp_wire_le16_to_cpu(header->offset) +
			     esp_wire_le16_to_cpu(header->len);
		size_t span = SPIHD_WIRE_SPAN(len);

		packet = spihd_alloc_skb(len);
		if (!packet) {
			skb_queue_purge(&packets);
			ret = -ENOMEM;
			goto out;
		}
		memcpy(skb_put(packet, len), skb->data + pos, len);
		skb_queue_tail(&packets, packet);
		pos += span;
	}
	while ((packet = skb_dequeue(&packets)) != NULL) {
		ret = spihd_process_rx_buf(packet);
		if (ret) {
			dev_kfree_skb_any(packet);
			skb_queue_purge(&packets);
			goto out;
		}
	}
out:
	dev_kfree_skb_any(skb);
	return ret;
}

static int spihd_receive_one(void)
{
	struct sk_buff *skb;
	u32 status_word;
	u32 pending;
	u32 status;
	int ret;

	if (!spi_hd.spi)
		return -ENODEV;

	if (spihd_use_tx_len_mirror())
		ret = spihd_read_tx_len_mirrored(&status_word);
	else if (spihd_full_duplex() &&
		 spi_max_transfer_size(spi_hd.spi) >= 7 &&
		 spi_max_message_size(spi_hd.spi) >= 14)
		ret = spihd_read_tx_len_batched(&status_word);
	else
		ret = spihd_read_reg_stable(SPIHD_REG_TX_BUF_LEN, &status_word);
	if (ret)
		return ret;

	status = status_word & SPIHD_INT_MASK;
	status_word &= SPIHD_TX_BUF_LEN_MASK;
	pending = (status_word - spi_hd.rx_byte_count) & SPIHD_TX_BUF_LEN_MASK;
	if (pending > spihd_rx_limit || (pending & (SPIHD_FRAME_ALIGNMENT - 1))) {
		esp_err("SPI-HD RX length %u exceeds %u\n",
			pending, spihd_rx_limit);
		return -EMSGSIZE;
	}
	/* Append DMA completion clears DataReady; status and idle still ACK. */
	if (spihd_rx_ack_required(status, pending)) {
		ret = spihd_command(SPIHD_CMD_INT1);
		if (ret) {
			spi_hd.rx_inflight = true;
			return ret;
		}
		/* The ACK may have removed the only notification of pending data. */
		if (pending)
			spi_hd.rx_inflight = true;
	}
	if (!pending)
		return status ? 0 : -EAGAIN;

	skb = spihd_alloc_skb(pending);
	if (!skb)
		return -ENOMEM;

	skb_put(skb, pending);
	ret = spihd_rddma(skb->data, pending);
	if (ret) {
		dev_kfree_skb_any(skb);
		return ret;
	}

	spi_hd.rx_byte_count =
		(spi_hd.rx_byte_count + pending) & SPIHD_TX_BUF_LEN_MASK;

	ret = spihd_process_rx_batch(skb);
	if (ret)
		spi_hd.rx_inflight = true;
	return ret;
}

static int spihd_data_ready(void)
{
	int value;

	if (!spi_hd.data_ready_gpio)
		return -ENODEV;

	value = gpiod_get_value_cansleep(spi_hd.data_ready_gpio);
	if (value < 0)
		esp_err("Failed to read SPI-HD data-ready GPIO: %d\n", value);
	return value;
}

/* The generic recovery scheduler quarantines TX for its quiet delay.
 * Stop draining valid RX descriptors at once so firmware backpressure,
 * rather than receiver drops, bridges that live-session interval.
 */
static bool spihd_live_recovery_pending(void)
{
	struct esp_adapter *adapter = spi_hd.adapter;

	return adapter &&
		atomic_read(&adapter->state) < ESP_CONTEXT_READY &&
		test_bit(ESP_INIT_DONE, &adapter->state_flags) &&
		test_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags) &&
		!test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags);
}

static bool spihd_tx_pair_compatible(const struct sk_buff *first,
				     const struct sk_buff *next)
{
	const struct esp_payload_header *a, *b;
	u16 ao, bo;

	if (!first || !next || first->len < sizeof(*a) || next->len < sizeof(*b) ||
		first->len > SPI_HD_BUF_SIZE || next->len > SPI_HD_BUF_SIZE ||
		SPIHD_WIRE_SPAN(first->len) + SPIHD_WIRE_SPAN(next->len) >
		SPI_HD_TX_BATCH_MAX)
		return false;
	a = (const void *)first->data;
	b = (const void *)next->data;
	ao = esp_wire_le16_to_cpu(a->offset);
	bo = esp_wire_le16_to_cpu(b->offset);
	if (!ESP_OFFSET_VALID(ao) || !ESP_OFFSET_VALID(bo) ||
		!a->len || !b->len ||
		ao + esp_wire_le16_to_cpu(a->len) != first->len ||
		bo + esp_wire_le16_to_cpu(b->len) != next->len ||
		a->if_type != b->if_type || a->if_num != b->if_num ||
		a->packet_type != PACKET_TYPE_DATA || b->packet_type != PACKET_TYPE_DATA)
		return false;
	if (a->if_type == ESP_STA_IF || a->if_type == ESP_AP_IF)
		return true;
	if (a->if_type == ESP_TEST_IF &&
		esp_wire_le16_to_cpu(a->len) >= sizeof(struct raw_tp_packet) &&
		esp_wire_le16_to_cpu(b->len) >= sizeof(struct raw_tp_packet)) {
		const struct raw_tp_packet *at = (const void *)(first->data + ao);
		const struct raw_tp_packet *bt = (const void *)(next->data + bo);

		return at->run_id == bt->run_id;
	}
	return false;
}

static int spihd_send_queued_frame(struct sk_buff *first, u32 *frame_count)
{
	struct sk_buff *next;
	struct sk_buff aggregate = {};
	struct sk_buff_head *queue = &spi_hd.tx_q[PRIO_Q_LOW];
	unsigned long flags;
	int ret;

	*frame_count = 1;
	if (!spi_hd.tx_batch_buf ||
	    READ_ONCE(spi_hd.tx_segment_max) < SPI_HD_TX_BATCH_MAX)
		return spihd_send_frame(first);
	spin_lock_irqsave(&queue->lock, flags);
	next = skb_peek(queue);
	if (!spihd_tx_pair_compatible(first, next)) {
		spin_unlock_irqrestore(&queue->lock, flags);
		return spihd_send_frame(first);
	}
	next = __skb_dequeue(queue);
	spin_unlock_irqrestore(&queue->lock, flags);
	if (atomic_read(&tx_pending) > 0)
		atomic_dec(&tx_pending);

#if TEST_RAW_TP
	if (((struct esp_payload_header *)next->data)->if_type == ESP_TEST_IF) {
		struct esp_payload_header *header = (void *)next->data;

		esp_raw_tp_set_seq(header, esp_raw_tp_tx_seq_get() + 1);
		if (spi_hd.adapter->capabilities & ESP_CHECKSUM_ENABLED) {
			header->checksum = 0;
			header->checksum = esp_wire_cpu_to_le16(compute_checksum(next->data, next->len));
		}
	}
#endif
	aggregate.data = spi_hd.tx_batch_buf;
	aggregate.len = SPIHD_WIRE_SPAN(first->len) + SPIHD_WIRE_SPAN(next->len);
	memcpy(aggregate.data, first->data, first->len);
	memset(aggregate.data + first->len, 0,
	       SPIHD_WIRE_SPAN(first->len) - first->len);
	memcpy(aggregate.data + SPIHD_WIRE_SPAN(first->len), next->data, next->len);
	memset(aggregate.data + SPIHD_WIRE_SPAN(first->len) + next->len, 0,
	       SPIHD_WIRE_SPAN(next->len) - next->len);
	*frame_count = 2;
	ret = spihd_send_frame(&aggregate);
	dev_kfree_skb_any(next);
	if (!ret)
		pr_info_once("SPI-HD TX batch: frames=2 bytes=%u\n", aggregate.len);
	return ret;
}

static void spihd_work(struct work_struct *work)
{
	struct sk_buff *skb;
	struct esp_payload_header *header;
	bool raw_tx;
	u32 raw_run_id;
	u32 frame_count;
	u8 cmd_code;
	u16 cmd_seq;
	bool is_cmd, is_stateful;
	bool progressed;
	unsigned int rx_batch, tx_batch;
	unsigned int rx_retries = 0;
	int ret, ready;

	if (!spihd_rx_enabled() || !spi_hd.spi ||
	    spihd_live_recovery_pending())
		return;

	/*
	 * RX and TX share one SPI controller/workqueue. Draining either
	 * source to empty can run forever under sustained opposite-direction
	 * traffic: cap each pass, then service the other side immediately.
	 * Bound each direction's work before servicing the other.
	 */
	for (;;) {
		progressed = false;
		for (rx_batch = 0; rx_batch < SPIHD_RX_WORK_QUOTA; rx_batch++) {
			if (!spihd_rx_enabled() ||
			    spihd_live_recovery_pending())
				return;
			ready = spihd_data_ready();
			if (ready < 0) {
				ret = ready;
				goto rx_error;
			}
			if (!ready)
				break;
			ret = spihd_receive_one();
			if (ret == -EAGAIN && !spi_hd.rx_inflight) {
				ready = spihd_data_ready();
				if (ready < 0) {
					ret = ready;
					goto rx_error;
				}
				if (!ready) {
					rx_retries = 0;
					break;
				}
				/* An asserted level may not produce a new edge. */
				if (++rx_retries <= 3) {
					usleep_range(100, 200);
					progressed = true;
					continue;
				}
			}
			if (ret)
				goto rx_error;
			rx_retries = 0;
			progressed = true;
		}

		if (READ_ONCE(spi_hd.state) != SPIHD_RUNNING || !spi_hd.adapter ||
		    atomic_read(&spi_hd.adapter->state) < ESP_CONTEXT_READY) {
			if (!progressed)
				return;
			continue;
		}

		for (tx_batch = 0; tx_batch < SPIHD_TX_WORK_QUOTA; tx_batch++) {
			if (!spihd_tx_allowed())
				return;
			skb = skb_dequeue(&spi_hd.tx_q[PRIO_Q_HIGH]);
			if (!skb)
				skb = skb_dequeue(&spi_hd.tx_q[PRIO_Q_MID]);
			if (!skb)
				skb = skb_dequeue(&spi_hd.tx_q[PRIO_Q_LOW]);
			if (!skb)
				break;
			progressed = true;

			if (atomic_read(&tx_pending) > 0)
				atomic_dec(&tx_pending);

			raw_tx = false;
			raw_run_id = 0;
			cmd_code = 0;
			cmd_seq = 0;
			is_cmd = esp_spi_get_cmd_info(skb, &cmd_code, &cmd_seq);
			is_stateful = esp_spi_is_stateful_packet(skb);
			header = (struct esp_payload_header *)skb->data;

#if TEST_RAW_TP
			if (header->if_type == ESP_TEST_IF) {
				u16 hdr_off = esp_wire_le16_to_cpu(header->offset);

				raw_tx = true;
				if (hdr_off + sizeof(struct raw_tp_packet) <= skb->len) {
					struct raw_tp_packet *tp =
						(struct raw_tp_packet *)(skb->data + hdr_off);
					raw_run_id = esp_wire_le32_to_cpu(tp->run_id);
				}
				esp_raw_tp_set_seq(header, esp_raw_tp_tx_seq_get());
				if (spi_hd.adapter->capabilities & ESP_CHECKSUM_ENABLED) {
					header->checksum = 0;
					header->checksum = esp_wire_cpu_to_le16(
						compute_checksum(skb->data,
							esp_wire_le16_to_cpu(header->len) +
							esp_wire_le16_to_cpu(header->offset)));
				}
			}
#endif

			frame_count = 1;
			if (!spihd_tx_allowed())
				ret = -ESHUTDOWN;
			else
				ret = spihd_send_queued_frame(skb, &frame_count);
			dev_kfree_skb_any(skb);
			if (ret) {
#if TEST_RAW_TP
				if (raw_tx)
					esp_raw_tp_tx_failed(frame_count);
#endif
				if (is_cmd)
					esp_cmd_transport_failed(spi_hd.adapter,
								 cmd_code, cmd_seq, ret);
				esp_err("SPI-HD transmit failed: %d\n", ret);
				if (is_stateful) {
					esp_schedule_fw_reset_recovery(spi_hd.adapter);
					esp_request_firmware_restart(spi_hd.adapter);
				} else {
					esp_schedule_transport_recovery(spi_hd.adapter);
				}
				return;
			}

#if TEST_RAW_TP
			if (raw_tx)
				esp_raw_tp_tx_complete(raw_run_id, frame_count);
#endif
		}
		if (!progressed)
			break;
	}
	return;

rx_error:
	esp_err("SPI-HD receive failed: %d\n", ret);
	esp_schedule_transport_recovery(spi_hd.adapter);
}

static irqreturn_t spihd_data_ready_irq(int irq, void *dev_id)
{
	spihd_kick();
	return IRQ_HANDLED;
}

static struct sk_buff *spihd_read_packet(struct esp_adapter *adapter)
{
	struct sk_buff *skb;

	if (!adapter || adapter != spi_hd.adapter)
		return NULL;

	skb = skb_dequeue(&spi_hd.rx_q[PRIO_Q_HIGH]);
	if (!skb)
		skb = skb_dequeue(&spi_hd.rx_q[PRIO_Q_MID]);
	if (!skb)
		skb = skb_dequeue(&spi_hd.rx_q[PRIO_Q_LOW]);
	return skb;
}

static int spihd_write_packet(struct esp_adapter *adapter, struct sk_buff *skb)
{
	struct esp_payload_header *header;
	unsigned long flags;
	u8 prio;
	int ret;

	if (!adapter || adapter != spi_hd.adapter || !skb ||
	    skb->len < sizeof(*header)) {
		ret = -EINVAL;
		goto drop;
	}
	if (skb->len > SPI_HD_BUF_SIZE) {
		ret = -EMSGSIZE;
		goto drop;
	}
	if (skb_is_nonlinear(skb) && skb_linearize(skb)) {
		ret = -ENOMEM;
		goto drop;
	}
	header = (struct esp_payload_header *)skb->data;
	if (header->if_type == ESP_INTERNAL_IF)
		prio = PRIO_Q_HIGH;
	else if (header->if_type == ESP_HCI_IF || header->if_type == ESP_RCP_IF)
		prio = PRIO_Q_MID;
	else
		prio = PRIO_Q_LOW;

	spin_lock_irqsave(&spihd_state_lock, flags);
	if (spi_hd.state != SPIHD_RUNNING || spi_hd.reset_pending || !spi_hd.workqueue ||
	    atomic_read(&adapter->state) < ESP_CONTEXT_READY ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
	    (test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_DEINIT, &adapter->state_flags) &&
	     !test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags))) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	if (atomic_read(&tx_pending) >= SPIHD_TX_MAX_PENDING) {
		ret = -EBUSY;
		goto unlock;
	}
	atomic_inc(&tx_pending);
	skb_queue_tail(&spi_hd.tx_q[prio], skb);
	queue_work(spi_hd.workqueue, &spi_hd.work);
	spin_unlock_irqrestore(&spihd_state_lock, flags);
	return 0;

unlock:
	spin_unlock_irqrestore(&spihd_state_lock, flags);
drop:
	dev_kfree_skb_any(skb);
	return ret;
}

static int spihd_hw_reset(void)
{
	int marker_ret;

	if (!spi_hd.reset_gpio)
		return -EOPNOTSUPP;

	/*
	 * Invalidate the previous incarnation's ready byte before asserting EN.
	 * The Slave-HD shared buffer can otherwise remain readable long enough
	 * after reset for probe to accept a stale 0xEE and publish DATAPATH_ON
	 * before the rebooting firmware clears its registers.  A fresh firmware
	 * incarnation rewrites SLAVE_READY=0xEE at the end of SPI-HD init.
	 */
	marker_ret = spihd_write_reg_lines(SPIHD_REG_SLAVE_READY, 0, 1);
	if (marker_ret)
		esp_warn("SPI-HD reset generation marker write failed: %d\n",
			 marker_ret);

	return esp_spi_pulse_reset_gpio(spi_hd.reset_gpio,
				       &spi_hd.reset_gpio_driven, 2000, 3000, 300);
}

/* Ordered behind the data worker: never synchronously cancel that worker
 * from a reset request made by the worker itself. */
static void spihd_reset_work(struct work_struct *work)
{
	struct esp_adapter *adapter = spi_hd.adapter;
	unsigned long flags;
	int ret;

	mutex_lock(&spihd_recovery_lock);
	if (!adapter || READ_ONCE(spi_hd.state) == SPIHD_STOPPED ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
	    test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags))
		goto out;

	spihd_purge_queues();
	spi_hd.rx_byte_count = 0;
	spi_hd.tx_segment_count = 0;
	spi_hd.rx_credit_limit = 0;
	spi_hd.tx_inflight = false;
	spi_hd.rx_inflight = false;
	spi_hd.data_lines = 1;
	spi_hd.spi_clk_mhz = spi_hd.requested_clk_mhz;
	spi_hd.control_max_hz = SPI_HD_INITIAL_CLK_MHZ * NUMBER_1M;
	spi_hd.payload_max_hz = 0;
	ret = spihd_hw_reset();
	if (ret)
		esp_err("SPI-HD target reset failed: %d\n", ret);
	else
		clear_bit(ESP_FW_RESTART_NEEDED, &adapter->state_flags);
out:
	spin_lock_irqsave(&spihd_state_lock, flags);
	WRITE_ONCE(spi_hd.reset_pending, false);
	/* Restart the quiet interval only after the physical pulse settles.
	 * Removal closes this gate before draining either worker. */
	if (adapter && spi_hd.state != SPIHD_STOPPED &&
	    !test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) &&
	    !test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags))
		mod_delayed_work(system_wq, &adapter->fw_recovery_work,
				 msecs_to_jiffies(ESP_FW_RECOVERY_QUIET_MS));
	spin_unlock_irqrestore(&spihd_state_lock, flags);
	mutex_unlock(&spihd_recovery_lock);
}

static int spihd_reset_target(struct esp_adapter *adapter)
{
	unsigned long flags;
	int ret = 0;

	if (!adapter || adapter != spi_hd.adapter)
		return -EINVAL;
	spin_lock_irqsave(&spihd_state_lock, flags);
	if (spi_hd.state == SPIHD_STOPPED || !spi_hd.workqueue ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) ||
	    test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags)) {
		ret = -ESHUTDOWN;
	} else if (!spi_hd.reset_gpio) {
		ret = -EOPNOTSUPP;
	} else if (!spi_hd.reset_pending) {
		WRITE_ONCE(spi_hd.reset_pending, true);
		WRITE_ONCE(spi_hd.state, SPIHD_QUIESCING);
		queue_work(spi_hd.workqueue, &spi_hd.reset_work);
	}
	spin_unlock_irqrestore(&spihd_state_lock, flags);
	return ret;
}

static int spihd_quiesce_for_fw_reset(struct esp_adapter *adapter)
{
	if (!adapter || adapter != spi_hd.adapter)
		return -EINVAL;

	spihd_quiesce(SPIHD_QUIESCING);
	spihd_purge_queues();
	return 0;
}

/*
 * Resume only a provably clean live session: no ambiguous DMA transaction,
 * coherent cumulative counters, and an unchanged firmware generation.
 * Unknown WRDMA/RDDMA completion or impossible counters return -EUCLEAN,
 * which causes the caller to initiate a counted firmware reset. Return 1
 * only for a clean resume, or 0 for a firmware that is not yet SLAVE_READY.
 */
static int spihd_resync_live_session(struct esp_adapter *adapter)
{
	u32 ready = 0, rx_credits = 0, tx_bytes = 0, lo, hi, check_lo, check_hi;
	s32 credit_delta;
	u32 pending;
	int ret;

	ret = spihd_read_reg_stable_lines(SPIHD_REG_SLAVE_READY, &ready, 1);
	if (ret)
		return ret;
	if ((ready & 0xff) != SPIHD_STATE_SLAVE_READY)
		return 0;
	ret = spihd_read_session(&lo, &hi);
	if (ret)
		return ret;
	if (lo != spi_hd.session_lo || hi != spi_hd.session_hi)
		return 0;

	/* The SPI core can report an error after transmitting any prefix.
	 * Without an on-wire transaction/sequence ACK, neither WR_END nor INT0
	 * can safely complete an in-flight WRDMA/RDDMA. Treat both directions
	 * as uncertain and force a new firmware incarnation instead.
	 */
	if (spi_hd.tx_inflight || spi_hd.rx_inflight)
		return -EUCLEAN;

	ret = spihd_read_reg_stable_lines(SPIHD_REG_RX_BUF_LEN, &rx_credits, 1);
	if (ret)
		return ret;
	ret = spihd_read_reg_stable_lines(SPIHD_REG_TX_BUF_LEN, &tx_bytes, 1);
	if (ret)
		return ret;

	ret = spihd_read_session(&check_lo, &check_hi);
	if (ret)
		return ret;
	if (lo != check_lo || hi != check_hi)
		return -EAGAIN;

	tx_bytes &= SPIHD_TX_BUF_LEN_MASK;
	credit_delta = (s32)(rx_credits - spi_hd.tx_segment_count);
	pending = (tx_bytes - spi_hd.rx_byte_count) & SPIHD_TX_BUF_LEN_MASK;
	if (credit_delta < 0 || (u32)credit_delta > spi_hd.rx_credit_window ||
	    pending > spihd_rx_limit) {
		esp_warn("SPI-HD live resync rejected: rx_credits=%u sent=%u tx_bytes=0x%06x read=0x%06x\n",
			 rx_credits, spi_hd.tx_segment_count, tx_bytes,
			 spi_hd.rx_byte_count);
		/* SLAVE_READY is still asserted: these are NOT new-generation
		 * counters. Reopening this instance with host counts reset to
		 * zero will loop forever and may duplicate or corrupt traffic.
		 */
		return -EUCLEAN;
	}

	/* A zero sentinel is not safe after the cumulative count wraps. */
	spi_hd.rx_credit_limit = spi_hd.tx_segment_count;
	spihd_set_state(SPIHD_RUNNING);
	atomic_set(&adapter->state, ESP_CONTEXT_READY);
	esp_info("SPI-HD live resync: lines=%u rx_credits=%u sent=%u e2h_pending=%u\n",
		 spi_hd.data_lines, rx_credits, spi_hd.tx_segment_count,
		 pending);
	return 1;
}

static int spihd_recover_transport_locked(struct esp_adapter *adapter)
{
	int ret;

	if (!adapter || adapter != spi_hd.adapter || !spi_hd.spi)
		return -ENODEV;
	/* Once OPEN succeeded, keep the bootstrap worker alive rather than
	 * zeroing live counters or issuing OPEN again while boot is pending. */
	if (READ_ONCE(spi_hd.state) == SPIHD_BOOTSTRAP) {
		u32 lo, hi;

		ret = spihd_read_session(&lo, &hi);
		if (!ret && lo == spi_hd.session_lo && hi == spi_hd.session_hi) {
			if (time_before(jiffies, spi_hd.boot_deadline)) {
				spihd_kick();
				return 0;
			}
			esp_schedule_fw_reset_recovery(adapter);
			esp_request_firmware_restart(adapter);
			return -ETIMEDOUT;
		}
		if (ret)
			return ret;
	}
	esp_dbg("SPI-HD recovery: lines=%u state=%u tx-inflight=%u rx-inflight=%u sent=%u consumed=0x%06x credits=%u queued=%d\n",
		spi_hd.data_lines, READ_ONCE(spi_hd.state),
		spi_hd.tx_inflight, spi_hd.rx_inflight,
		spi_hd.tx_segment_count, spi_hd.rx_byte_count,
		spi_hd.rx_credit_limit, atomic_read(&tx_pending));

	if (test_bit(ESP_INIT_DONE, &adapter->state_flags) &&
	    !test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags)) {
		spihd_quiesce(SPIHD_RECOVERING);
		ret = spihd_resync_live_session(adapter);
		/* The generic recovery worker invokes the rearm hook after clearing
		 * ESP_FW_RECOVERY_PENDING; queuing here can lose the last wakeup.
		 */
		if (ret == -EUCLEAN) {
			/* Reset ownership, counters and the firmware together. A live
			 * WRDMA/RDDMA has no safe partial-transaction replay.
			 * Use the common counted reboot path so a missing new boot TLV
			 * cannot yield an unbounded local-reset loop.
			 */
			esp_warn("SPI-HD ambiguous DMA/counter state; requesting firmware reset\n");
			esp_schedule_fw_reset_recovery(adapter);
			esp_request_firmware_restart(adapter);
			return -EAGAIN;
		} else if (ret) {
			return ret;
		}
		/* The old session is gone. Quarantine old queues, not the new
		 * firmware: the common boot path will reconstruct the interfaces. */
		esp_schedule_fw_reset_recovery(adapter);
		spihd_purge_queues();
	}

	/*
	 * A spontaneous firmware reset returns the SPI-HD command decoder to
	 * single-line bring-up even though the host may have been operating in
	 * QIO. Always rebase both cumulative counters and line width before
	 * polling SLAVE_READY; the new boot event will switch us back to QIO.
	 */
	spihd_quiesce(SPIHD_RECOVERING);
	spi_hd.data_lines = 1;
	spi_hd.rx_byte_count = 0;
	spi_hd.tx_segment_count = 0;
	spi_hd.rx_credit_limit = 0;
	spi_hd.tx_inflight = false;
	spi_hd.rx_inflight = false;

	spi_hd.spi_clk_mhz = spi_hd.requested_clk_mhz;
	spi_hd.control_max_hz = SPI_HD_INITIAL_CLK_MHZ * NUMBER_1M;
	spi_hd.payload_max_hz = 0;
	ret = spihd_wait_slave_ready();
	if (ret)
		goto reset_if_needed;
	ret = spihd_negotiate_limits();
	if (ret)
		goto reset_if_needed;
	ret = spihd_open_slave_datapath();
	if (ret)
		goto reset_if_needed;

	spi_hd.boot_deadline = jiffies + msecs_to_jiffies(SPIHD_INITIAL_BOOT_TIMEOUT_MS);
	spihd_set_state(SPIHD_BOOTSTRAP);
	spihd_kick();
	return 0;

reset_if_needed:
	/* An already-open or unreachable peer cannot be adopted by zeroing
	 * counters. Use the common bounded reset path, not unbounded OPEN. */
	if (ret == -EBUSY || ret == -ETIMEDOUT || ret == -EIO) {
		esp_schedule_fw_reset_recovery(adapter);
		esp_request_firmware_restart(adapter);
	}
	return ret;
}

static int spihd_recover_transport(struct esp_adapter *adapter)
{
	int ret;

	mutex_lock(&spihd_recovery_lock);
	if (READ_ONCE(spi_hd.reset_pending))
		ret = -EAGAIN;
	else if (READ_ONCE(spi_hd.state) == SPIHD_STOPPED)
		ret = -ESHUTDOWN;
	else
		ret = spihd_recover_transport_locked(adapter);
	mutex_unlock(&spihd_recovery_lock);
	return ret;
}

static int spihd_reinit_after_fw_reset(struct esp_adapter *adapter)
{
	if (!adapter || adapter != spi_hd.adapter)
		return -EINVAL;
	if (READ_ONCE(spi_hd.reset_pending) || READ_ONCE(spi_hd.state) == SPIHD_STOPPED)
		return -ESHUTDOWN;

	spihd_set_state(SPIHD_RUNNING);
	atomic_set(&adapter->state, ESP_CONTEXT_READY);
	spihd_kick();
	return 0;
}

static void spihd_flush_bt_traffic(struct esp_adapter *adapter)
{
	struct sk_buff *skb, *tmp;
	struct sk_buff_head free_q;
	unsigned long flags;
	u8 q;

	if (!adapter || adapter != spi_hd.adapter)
		return;

	__skb_queue_head_init(&free_q);

	for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
		spin_lock_irqsave(&spi_hd.tx_q[q].lock, flags);
		skb_queue_walk_safe(&spi_hd.tx_q[q], skb, tmp) {
			struct esp_payload_header *header;

			if (skb->len < sizeof(*header))
				continue;
			header = (struct esp_payload_header *)skb->data;
			if (header->if_type != ESP_HCI_IF)
				continue;

			__skb_unlink(skb, &spi_hd.tx_q[q]);
			if (atomic_read(&tx_pending) > 0)
				atomic_dec(&tx_pending);
			__skb_queue_tail(&free_q, skb);
		}
		spin_unlock_irqrestore(&spi_hd.tx_q[q].lock, flags);
	}

	/*
	 * Wait out a frame the worker may already own before purging received
	 * HCI data. A transaction already on the wire cannot be revoked.
	 */
	if (spi_hd.workqueue)
		cancel_work_sync(&spi_hd.work);

	for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
		spin_lock_irqsave(&spi_hd.rx_q[q].lock, flags);
		skb_queue_walk_safe(&spi_hd.rx_q[q], skb, tmp) {
			struct esp_payload_header *header;

			if (skb->len < sizeof(*header))
				continue;
			header = (struct esp_payload_header *)skb->data;
			if (header->if_type != ESP_HCI_IF)
				continue;

			__skb_unlink(skb, &spi_hd.rx_q[q]);
			__skb_queue_tail(&free_q, skb);
		}
		spin_unlock_irqrestore(&spi_hd.rx_q[q].lock, flags);
	}

	while ((skb = __skb_dequeue(&free_q)) != NULL)
		dev_kfree_skb_any(skb);

	/* Recheck the GPIO in the worker, including read errors. */
	spihd_kick();
}

static void spihd_resume_after_live_recovery(struct esp_adapter *adapter)
{
	if (adapter != spi_hd.adapter)
		return;
	/* Queue unconditionally: either a pending TX skb or an already-high
	 * Data Ready level may need service without another rising IRQ edge.
	 */
	spihd_kick();
}

static struct esp_if_ops spihd_if_ops = {
	.rx_during_live_recovery = true,
	.resume_after_live_recovery = spihd_resume_after_live_recovery,
	.read = spihd_read_packet,
	.write = spihd_write_packet,
	.alloc_skb = spihd_alloc_skb,
	.quiesce_for_fw_reset = spihd_quiesce_for_fw_reset,
	.reinit_after_fw_reset = spihd_reinit_after_fw_reset,
	.reset_target = spihd_reset_target,
	.recover_transport = spihd_recover_transport,
	.flush_bt_traffic = spihd_flush_bt_traffic,
};

static void spihd_release_reset_gpio(void)
{
	if (!spi_hd.reset_gpio)
		return;

	if (spi_hd.reset_gpio_driven &&
	    gpiod_direction_input(spi_hd.reset_gpio))
		esp_warn("Failed to release SPI-HD reset GPIO\n");

	spi_hd.reset_gpio_driven = false;
	spi_hd.reset_gpio = NULL;
}

static int spihd_probe(struct spi_device *spi)
{
	u32 board_max_hz;
	int ret;

	if (!spi || !spi_hd.adapter || spi_hd.spi)
		return -ENODEV;

	board_max_hz = spi->max_speed_hz;
	if (!board_max_hz) {
		esp_err("SPI-HD requires a board maximum clock rate\n");
		return -EINVAL;
	}
	spi_hd.spi_max_hz = min_t(u32, board_max_hz,
				  SPI_HD_MAX_CLK_MHZ * NUMBER_1M);
	if (SPIHD_CONTROLLER(spi)->max_speed_hz)
		spi_hd.spi_max_hz = min(spi_hd.spi_max_hz,
				       SPIHD_CONTROLLER(spi)->max_speed_hz);
	if (!spihd_bpw_supported(spi, 8) ||
	    (SPIHD_CONTROLLER(spi)->flags & (SPIHD_CTLR_NO_TX | SPIHD_CTLR_NO_RX))) {
		esp_err("SPI-HD requires 8-bit words and both TX/RX phases\n");
		return -EOPNOTSUPP;
	}
	if (!SPIHD_CONTROLLER(spi)->transfer && !SPIHD_CONTROLLER(spi)->transfer_one &&
	    !SPIHD_CONTROLLER(spi)->transfer_one_message) {
		esp_err("SPI-HD requires generic SPI messages; SPI-memory-only controllers need another backend\n");
		return -EOPNOTSUPP;
	}
	/* Reject flags that change the wire order, share data lanes or remove
	 * frame boundaries. The SPI core validates all other mode flags. */
	if (spi->mode & (SPI_LSB_FIRST | SPI_3WIRE | SPI_LOOP | SPI_NO_CS
#ifdef SPI_CS_WORD
			 | SPI_CS_WORD
#endif
			 )) {
		esp_err("Unsupported SPI-HD mode flags 0x%x\n", spi->mode);
		return -EINVAL;
	}

	spi->max_speed_hz = spi_hd.spi_max_hz;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	esp_info("SPI-HD active mode: %u\n",
		 (unsigned int)(spi->mode & (SPI_CPOL | SPI_CPHA)));
	spi_hd.spi = spi;
	spihd_set_state(SPIHD_INITIALIZING);
	spi_set_drvdata(spi, &spi_hd);

	esp_info("SPI-HD controller=%s device=%s mode=0x%x flags=0x%x bpw-mask=0x%x board-max=%uHz bus-max=%uHz max-transfer=%zu max-message=%zu quad=%u tx-quad=%u rx-quad=%u full-duplex=%u\n",
		 dev_name(&SPIHD_CONTROLLER(spi)->dev), dev_name(&spi->dev),
		 spi->mode, SPIHD_CONTROLLER(spi)->flags,
		 SPIHD_CONTROLLER(spi)->bits_per_word_mask, board_max_hz,
		 spi_hd.spi_max_hz, spi_max_transfer_size(spi),
		 spi_max_message_size(spi), spihd_quad_available(spi),
		 !!(spi->mode & SPI_TX_QUAD), !!(spi->mode & SPI_RX_QUAD),
		 spihd_full_duplex());
	/* A pre-booted SPI-HD slave does not need a reset GPIO for normal
	 * transfers. Without one, hardware-reset recovery is unavailable.
	 */
	spi_hd.reset_gpio = devm_gpiod_get_optional(&spi->dev, "reset",
						GPIOD_IN);
	if (IS_ERR(spi_hd.reset_gpio)) {
		ret = PTR_ERR(spi_hd.reset_gpio);
		spi_hd.reset_gpio = NULL;
		goto fail;
	}

	spi_hd.data_ready_gpio =
		devm_gpiod_get(&spi->dev, "data-ready", GPIOD_IN);
	if (IS_ERR(spi_hd.data_ready_gpio)) {
		ret = PTR_ERR(spi_hd.data_ready_gpio);
		spi_hd.data_ready_gpio = NULL;
		goto fail;
	}

	spi_hd.data_ready_irq = gpiod_to_irq(spi_hd.data_ready_gpio);
	if (spi_hd.data_ready_irq < 0) {
		ret = spi_hd.data_ready_irq;
		goto fail;
	}
	esp_dbg("SPI-HD GPIO: reset-present=%u reset-active-low=%u ready-active-low=%u ready-can-sleep=%u irq=%d edge=%s\n",
		!!spi_hd.reset_gpio,
		spi_hd.reset_gpio && gpiod_is_active_low(spi_hd.reset_gpio),
		gpiod_is_active_low(spi_hd.data_ready_gpio),
		gpiod_cansleep(spi_hd.data_ready_gpio), spi_hd.data_ready_irq,
		gpiod_is_active_low(spi_hd.data_ready_gpio) ? "falling" : "rising");

	ret = devm_request_irq(&spi->dev, spi_hd.data_ready_irq,
			       spihd_data_ready_irq,
			       esp_spi_gpio_irq_flags(spi_hd.data_ready_gpio),
			       "ESP_SPI_HD_DATA_READY", spi);
	if (ret)
		goto fail;

	clear_bit(ESP_TRANSPORT_REMOVING, &spi_hd.adapter->state_flags);
	clear_bit(ESP_CLEANUP_IN_PROGRESS, &spi_hd.adapter->state_flags);

	spi_hd.data_lines = 1;
	spi_hd.spi_clk_mhz = spi_hd.requested_clk_mhz;
	spi_hd.control_max_hz = SPI_HD_INITIAL_CLK_MHZ * NUMBER_1M;
	spi_hd.payload_max_hz = 0;
	if (!spi_hd.reset_gpio) {
		esp_warn("SPI-HD reset GPIO absent: expecting a booted slave; hardware-reset recovery unavailable\n");
	} else {
		ret = spihd_hw_reset();
		if (ret)
			goto fail;
	}

	ret = spihd_wait_slave_ready();
	if (ret)
		goto fail;

	ret = spihd_negotiate_limits();
	if (ret)
		goto fail;

	/*
	 * Initialize the counters before opening the slave datapath. An
	 * unconditional worker kick after publishing BOOTSTRAP handles an
	 * already asserted Data Ready level.
	 */
	spi_hd.rx_byte_count = 0;
	spi_hd.tx_segment_count = 0;
	spi_hd.rx_credit_limit = 0;
	spi_hd.tx_inflight = false;
	spi_hd.rx_inflight = false;
	spi_hd.logged_sclk_hz = 0;
	spi_hd.adapter->dev = &spi->dev;
	clear_bit(ESP_INIT_DONE, &spi_hd.adapter->state_flags);
	clear_bit(ESP_FW_RECOVERY_PENDING, &spi_hd.adapter->state_flags);
	clear_bit(ESP_FW_RESET_EXPECTED, &spi_hd.adapter->state_flags);
	clear_bit(ESP_FW_RESTART_NEEDED, &spi_hd.adapter->state_flags);
	atomic_set(&spi_hd.adapter->state, ESP_CONTEXT_RX_READY);
	ret = spihd_open_slave_datapath();
	if (ret)
		goto fail;

	spi_hd.boot_deadline = jiffies + msecs_to_jiffies(SPIHD_INITIAL_BOOT_TIMEOUT_MS);
	spihd_set_state(SPIHD_BOOTSTRAP);

	esp_info("ESP SPI-HD bound: clock=%uHz tx-segment=%u slave-tx-max=%u\n",
		 spihd_effective_speed_hz(), spi_hd.tx_segment_max, spihd_rx_limit);

	spihd_kick();
	/* Allow initial radio initialization to finish before recovery. */
	schedule_delayed_work(&spi_hd.adapter->fw_recovery_work,
			      msecs_to_jiffies(SPIHD_INITIAL_BOOT_TIMEOUT_MS));
	return 0;

fail:
	spihd_quiesce(SPIHD_STOPPED);
	set_bit(ESP_TRANSPORT_REMOVING, &spi_hd.adapter->state_flags);
	set_bit(ESP_CLEANUP_IN_PROGRESS, &spi_hd.adapter->state_flags);
	atomic_set(&spi_hd.adapter->state, ESP_CONTEXT_DISABLED);
	spihd_release_reset_gpio();
	spi_hd.data_ready_gpio = NULL;
	spi_hd.data_ready_irq = 0;
	spi_set_drvdata(spi, NULL);
	spi_hd.spi = NULL;
	if (spi_hd.adapter)
		spi_hd.adapter->dev = NULL;
	return ret;
}

static void spihd_remove_common(struct spi_device *spi)
{
	struct esp_adapter *adapter = spi_hd.adapter;

	spihd_set_state(SPIHD_STOPPED);
	if (adapter) {
		set_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags);
		set_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
		clear_bit(ESP_INIT_DONE, &adapter->state_flags);
		atomic_set(&adapter->state, ESP_CONTEXT_DISABLED);
		cancel_delayed_work_sync(&adapter->fw_recovery_work);
	}

	if (spi_hd.workqueue) {
		cancel_work_sync(&spi_hd.work);
		cancel_work_sync(&spi_hd.reset_work);
	}
	WRITE_ONCE(spi_hd.reset_pending, false);
	spihd_purge_queues();

	if (adapter && adapter->if_rx_workqueue)
		cancel_work_sync(&adapter->if_rx_work);
	if (adapter) {
		esp_remove_card(adapter, false);
		adapter->dev = NULL;
	}

	spihd_release_reset_gpio();
	spi_hd.data_ready_gpio = NULL;
	spi_hd.data_ready_irq = 0;
	spi_hd.spi = NULL;
	spi_set_drvdata(spi, NULL);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)
static int spihd_remove(struct spi_device *spi)
{
	spihd_remove_common(spi);
	return 0;
}
#else
static void spihd_remove(struct spi_device *spi)
{
	spihd_remove_common(spi);
}
#endif

static const struct of_device_id spihd_of_match[] = {
	{ .compatible = "espressif,esp32-spi-hd" },
	{}
};
MODULE_DEVICE_TABLE(of, spihd_of_match);

static const struct spi_device_id spihd_id[] = {
	{ "esp32-spi-hd", 0 },
	{}
};
MODULE_DEVICE_TABLE(spi, spihd_id);

static struct spi_driver spihd_driver = {
	.driver = {
		.name = "esp_spi_hd",
		.of_match_table = spihd_of_match,
	},
	.probe = spihd_probe,
	.remove = spihd_remove,
	.id_table = spihd_id,
};

int esp_validate_chipset(struct esp_adapter *adapter, u8 chipset)
{
	switch (chipset) {
	case ESP_FIRMWARE_CHIP_ESP32S2:
	case ESP_FIRMWARE_CHIP_ESP32S3:
	case ESP_FIRMWARE_CHIP_ESP32C2:
	case ESP_FIRMWARE_CHIP_ESP32C3:
	case ESP_FIRMWARE_CHIP_ESP32C5:
	case ESP_FIRMWARE_CHIP_ESP32C6:
	case ESP_FIRMWARE_CHIP_ESP32C61:
	case ESP_FIRMWARE_CHIP_ESP32S31:
		adapter->chipset = chipset;
		esp_info("Chipset=%s ID=%02x detected over SPI-HD/QIO\n",
			 esp_chipname_from_id(chipset), chipset);
		return 0;
	default:
		adapter->chipset = ESP_FIRMWARE_CHIP_UNRECOGNIZED;
		return -ENODEV;
	}
}

int esp_deinit_module(struct esp_adapter *adapter)
{
	u8 q;
	u8 iface;

	for (q = 0; q < MAX_PRIORITY_QUEUES; q++)
		skb_queue_purge(&spi_hd.tx_q[q]);
	atomic_set(&tx_pending, 0);

	for (iface = 0; iface < ESP_MAX_INTERFACE; iface++) {
		struct esp_wifi_device *priv = adapter->priv[iface];

		esp_mark_scan_done_and_disconnect(priv, true);
	}
	esp_remove_card(adapter, false);
	return 0;
}

int esp_adjust_spi_clock(struct esp_adapter *adapter, u8 spi_clk_mhz)
{
	if (!adapter || adapter != spi_hd.adapter || !spi_hd.spi)
		return -EINVAL;

	/* Boot events run on the common RX worker, not the SPI worker. */
	spihd_quiesce(SPIHD_QUIESCING);
	if (READ_ONCE(spi_hd.state) == SPIHD_STOPPED ||
	    READ_ONCE(spi_hd.reset_pending) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return -ESHUTDOWN;
	spi_hd.spi_clk_mhz = spi_clk_mhz ?
		min_t(u32, spi_clk_mhz, SPI_HD_MAX_CLK_MHZ) :
		spi_hd.requested_clk_mhz;
	WRITE_ONCE(spi_hd.data_lines, spihd_quad_available(spi_hd.spi) ? 4 : 1);
	spihd_set_state(SPIHD_RUNNING);
	esp_info("SPI-HD post-boot clock: advertised=%uMHz effective=%uHz dt_limit=%uHz\n",
		 spi_clk_mhz, spihd_effective_speed_hz(), spi_hd.spi_max_hz);
	spihd_kick();
	return 0;
}

int generate_slave_intr(void *context, u8 data)
{
	return 0;
}

int esp_init_interface_layer(struct esp_adapter *adapter, u32 speed)
{
	u8 q;
	int ret;

	if (!adapter)
		return -EINVAL;

	memset(&spi_hd, 0, sizeof(spi_hd));
	spi_hd.adapter = adapter;
	spi_hd.requested_clk_mhz = speed ?
		min_t(u32, speed, SPI_HD_MAX_CLK_MHZ) :
		SPI_HD_INITIAL_CLK_MHZ;
	spi_hd.spi_clk_mhz = spi_hd.requested_clk_mhz;
	spi_hd.control_max_hz = SPI_HD_INITIAL_CLK_MHZ * NUMBER_1M;
	spi_hd.payload_max_hz = 0;
	spi_hd.data_lines = 1;
	spi_hd.sio_buf = esp_transport_alloc_aligned(spihd_control_offset() +
		2 * SPIHD_MAX_PHASES * spihd_control_stride(),
		spihd_control_alignment(), &spi_hd.sio_alloc);
	spi_hd.tx_batch_buf = esp_transport_alloc_aligned(SPI_HD_TX_BATCH_MAX,
		spihd_control_alignment(), &spi_hd.tx_batch_alloc);
	if (!spi_hd.sio_buf || !spi_hd.tx_batch_buf) {
		kfree(spi_hd.sio_alloc);
		kfree(spi_hd.tx_batch_alloc);
		spi_hd.sio_buf = NULL;
		spi_hd.tx_batch_buf = NULL;
		return -ENOMEM;
	}

	adapter->if_context = &spi_hd;
	adapter->if_ops = &spihd_if_ops;
	adapter->if_type = ESP_IF_TYPE_SPI;
	atomic_set(&tx_pending, 0);

	for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
		skb_queue_head_init(&spi_hd.tx_q[q]);
		skb_queue_head_init(&spi_hd.rx_q[q]);
	}

	spi_hd.workqueue =
		alloc_ordered_workqueue("ESP_SPI_HD_WORK",
					WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (!spi_hd.workqueue) {
		kfree(spi_hd.sio_alloc);
		kfree(spi_hd.tx_batch_alloc);
		spi_hd.tx_batch_buf = NULL;
		spi_hd.sio_buf = NULL;
		return -ENOMEM;
	}
	INIT_WORK(&spi_hd.work, spihd_work);
	INIT_WORK(&spi_hd.reset_work, spihd_reset_work);

	ret = spi_register_driver(&spihd_driver);
	if (ret) {
		destroy_workqueue(spi_hd.workqueue);
		spi_hd.workqueue = NULL;
		kfree(spi_hd.sio_alloc);
		kfree(spi_hd.tx_batch_alloc);
		spi_hd.tx_batch_buf = NULL;
		spi_hd.sio_buf = NULL;
		return ret;
	}
	spi_hd_driver_registered = true;
	return 0;
}

void esp_deinit_interface_layer(void)
{
	spihd_set_state(SPIHD_STOPPED);
	if (spi_hd_driver_registered) {
		spi_unregister_driver(&spihd_driver);
		spi_hd_driver_registered = false;
	}
	if (spi_hd.workqueue) {
		cancel_work_sync(&spi_hd.work);
		cancel_work_sync(&spi_hd.reset_work);
		spihd_purge_queues();
		destroy_workqueue(spi_hd.workqueue);
		spi_hd.workqueue = NULL;
	}
	kfree(spi_hd.sio_alloc);
	kfree(spi_hd.tx_batch_alloc);
	memset(&spi_hd, 0, sizeof(spi_hd));
}
