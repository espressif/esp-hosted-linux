// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESP-Hosted SPI half-duplex/QIO transport.
 *
 * The wire protocol follows ESP-IDF SPI Slave HD mode. Raspberry Pi 5 uses
 * RP1 SPI0 in DesignWare enhanced-frame mode:
 *
 * Bring-up follows the ESP SPI-HD reference flow in SIO. After the first
 * boot/capability frame, command stays x1 while address/data switch to x4:
 *
 *   command: 8-bit x1
 *   address: 8-bit x1 during bring-up, x4 after boot
 *   dummy:   8 clocks
 *   data:    x1 during bring-up, x4 after boot
 *
 * The Raspberry Pi DesignWare quad support under development in
 * raspberrypi/linux#7587 limits enhanced writes to the controller TX FIFO.
 * Host-to-ESP frames are therefore split into bounded WRDMA segments. The ESP
 * backend reassembles those segments before handing a complete Hosted frame to
 * the common firmware path. ESP-to-host RDDMA reads are not TX-FIFO limited.
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
#include "esp_spi_hd.h"
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

#define SPIHD_CMD_WRBUF                 0x01
#define SPIHD_CMD_RDBUF                 0x02
#define SPIHD_CMD_WRDMA                 0x03
#define SPIHD_CMD_RDDMA                 0x04
#define SPIHD_CMD_WR_END                0x07
#define SPIHD_CMD_INT0                  0x08
#define SPIHD_CMD_INT1                  0x09
#define SPIHD_QIO_MASK                  0xA0
#define SPIHD_QIO_DATA_ONLY_MASK        0x20

#define SPIHD_REG_SLAVE_READY           0x00
#define SPIHD_REG_MAX_TX_BUF_LEN        0x04
#define SPIHD_REG_MAX_RX_BUF_LEN        0x08
#define SPIHD_REG_TX_BUF_LEN            0x0C
#define SPIHD_REG_RX_BUF_LEN            0x10
#define SPIHD_REG_SLAVE_CTRL            0x14
#define SPIHD_REG_TX_LEN_MIRROR         0x18
#define SPIHD_REG_QIO_SCRATCH           0x20
#define SPIHD_REG_RX_DIAG_LEN           0x34
#define SPIHD_REG_RX_DIAG_BITS          0x38
#define SPIHD_REG_RX_DIAG_RAW           0x3C

#define SPIHD_STATE_SLAVE_READY         0xEE
#define SPIHD_CAP_TX_LEN_MIRROR          BIT(8)
#define SPIHD_CTRL_DATAPATH_ON          BIT(0)

#define SPIHD_TX_BUF_LEN_MASK           0x00FFFFFFU
#define SPIHD_INT_MASK                  (3U << 24)
#define SPIHD_READY_TIMEOUT_MS          5000U
#define SPIHD_INITIAL_BOOT_TIMEOUT_MS   8000U
#define SPIHD_READY_POLL_MS             100U
#define SPIHD_CREDIT_TIMEOUT_MS         200U
#define SPIHD_CREDIT_POLL_US            1000U
#define SPIHD_LIVE_CREDIT_MAX           32
#define SPIHD_TX_MAX_PENDING            128

static struct esp_spi_hd_context spi_hd;
static atomic_t tx_pending;
static volatile u8 data_path;
volatile u8 host_sleep;

/*
 * SPI-HD host-side service scheduling only. E2H RX work and H2E TX work
 * share the single ordered SPI worker; this is NOT a firmware-side setting.
 * C5/Pi5 full-speed hardware A/B: default 3:2, 2:2 symmetric, old 8:2.
 *
 * Read-only module arguments are optional startup overrides. The debugfs
 * worker_quotas file atomically updates both values without unloading the
 * driver, resetting the C5 or affecting SDIO/SPI/USB transports.
 */
static uint spi_hd_rx_work_quota = 3;
module_param(spi_hd_rx_work_quota, uint, 0444);
MODULE_PARM_DESC(spi_hd_rx_work_quota,
		 "Startup E2H receive work quota (1..8; default 3)");
static uint spi_hd_tx_work_quota = 2;
module_param(spi_hd_tx_work_quota, uint, 0444);
MODULE_PARM_DESC(spi_hd_tx_work_quota,
		 "Startup H2E transmit work quota (1..8; default 2)");

#define SPIHD_WORK_QUOTA_MAX 8U
#define SPIHD_TX_QUOTA_SHIFT 8U
#define SPIHD_RX_QUOTA_MASK 0xffU
/* Single aligned store means the worker never sees half of an update. */
static u16 spi_hd_work_quotas = (2U << SPIHD_TX_QUOTA_SHIFT) | 3U;

int esp_spi_hd_set_work_quotas(unsigned int rx_quota, unsigned int tx_quota)
{
	if (!rx_quota || rx_quota > SPIHD_WORK_QUOTA_MAX ||
	    !tx_quota || tx_quota > SPIHD_WORK_QUOTA_MAX)
		return -EINVAL;

	WRITE_ONCE(spi_hd_work_quotas,
		   (u16)((tx_quota << SPIHD_TX_QUOTA_SHIFT) | rx_quota));
	return 0;
}

void esp_spi_hd_get_work_quotas(unsigned int *rx_quota,
			       unsigned int *tx_quota)
{
	u16 quotas = READ_ONCE(spi_hd_work_quotas);

	*rx_quota = quotas & SPIHD_RX_QUOTA_MASK;
	*tx_quota = quotas >> SPIHD_TX_QUOTA_SHIFT;
}

static bool spi_hd_tx_batch;
module_param(spi_hd_tx_batch, bool, 0444);
MODULE_PARM_DESC(spi_hd_tx_batch, "Opt-in: combine two queued data frames in one WRDMA");

static uint spi_hd_tx_segment = SPI_HD_TX_SEGMENT_DEFAULT;
module_param(spi_hd_tx_segment, uint, 0644);
MODULE_PARM_DESC(spi_hd_tx_segment,
		 "Maximum bytes in one RP1 quad WRDMA segment (default 32)");

static bool spi_hd_force_sio;
module_param(spi_hd_force_sio, bool, 0644);
MODULE_PARM_DESC(spi_hd_force_sio,
		 "Keep SPI-HD address/data phases single-line after boot for bring-up isolation");

static bool spi_hd_force_sio_h2e;
module_param(spi_hd_force_sio_h2e, bool, 0644);
MODULE_PARM_DESC(spi_hd_force_sio_h2e,
		 "Keep host-to-ESP SPI-HD transactions single-line after QIO switch");

/* Diagnostic only: prove QIO raw DMA separately from Hosted control-plane TX.
 * No protocol changes; original global line-mode behavior is the default.
 */
static bool spi_hd_force_sio_control_h2e;
module_param(spi_hd_force_sio_control_h2e, bool, 0644);
MODULE_PARM_DESC(spi_hd_force_sio_control_h2e,
		 "Diagnostic: force all non-test Hosted H2E through SIO (default off; control/short frames always SIO)");

static bool spi_hd_force_sio_control_e2h = true;
module_param(spi_hd_force_sio_control_e2h, bool, 0644);
MODULE_PARM_DESC(spi_hd_force_sio_control_e2h,
		 "Keep E2H registers/acks and <=256-byte control reads SIO; bulk RDDMA uses QIO");

static bool spi_hd_force_sio_e2h;
module_param(spi_hd_force_sio_e2h, bool, 0644);
MODULE_PARM_DESC(spi_hd_force_sio_e2h,
		 "Keep ESP-to-host SPI-HD transactions single-line after QIO switch");

static bool spi_hd_force_sio_wrend;
module_param(spi_hd_force_sio_wrend, bool, 0644);
MODULE_PARM_DESC(spi_hd_force_sio_wrend,
		 "Send WR_END in SIO while leaving WRDMA in the selected H2E line mode");

static bool spi_hd_qio_tx_word32;
module_param(spi_hd_qio_tx_word32, bool, 0644);
MODULE_PARM_DESC(spi_hd_qio_tx_word32,
		 "Opt-in long QIO TX: pack four wire bytes into each 32-bit FIFO/DMA entry");

static bool spi_hd_qio_rx_word32;
module_param(spi_hd_qio_rx_word32, bool, 0644);
MODULE_PARM_DESC(spi_hd_qio_rx_word32,
		 "Opt-in long QIO RX: drain four wire bytes per 32-bit FIFO/DMA entry");

static bool spi_hd_wrdma_batch_end;
module_param(spi_hd_wrdma_batch_end, bool, 0644);
MODULE_PARM_DESC(spi_hd_wrdma_batch_end,
		 "Opt-in: submit long QIO WRDMA and SIO WR_END in one SPI message");

static uint spi_hd_rx_batch_bytes = SPI_HD_BUF_SIZE;
module_param(spi_hd_rx_batch_bytes, uint, 0644);
MODULE_PARM_DESC(spi_hd_rx_batch_bytes, "Maximum negotiated E2H DMA aggregate capacity (1600..6400)");
static u32 spihd_rx_limit = SPI_HD_BUF_SIZE;
static unsigned int spihd_rx_batch_logs;

static bool spi_hd_append_tx;
module_param(spi_hd_append_tx, bool, 0444);
MODULE_PARM_DESC(spi_hd_append_tx,
		 "Opt-in: slave append TX descriptors complete without segment-mode INT0");

static bool spi_hd_append_no_ack;
module_param(spi_hd_append_no_ack, bool, 0444);
MODULE_PARM_DESC(spi_hd_append_no_ack,
		 "Opt-in: append firmware clears data-ready on DMA completion without INT1");

static bool spi_hd_tx_len_mirror;
module_param(spi_hd_tx_len_mirror, bool, 0444);
MODULE_PARM_DESC(spi_hd_tx_len_mirror,
		 "Opt-in: use negotiated reverse-published TX length copies in one SIO read");
static bool spihd_tx_len_mirror_available;

static bool spi_hd_rddma_batch_end;
module_param(spi_hd_rddma_batch_end, bool, 0644);
MODULE_PARM_DESC(spi_hd_rddma_batch_end,
		 "Opt-in: submit long QIO RDDMA and SIO INT0 in one SPI message");

static bool spi_hd_rx_batch_control;
module_param(spi_hd_rx_batch_control, bool, 0644);
MODULE_PARM_DESC(spi_hd_rx_batch_control,
		 "Opt-in: batch stable length reads and long QIO INT1/RDDMA/INT0");

static bool spi_hd_timing_diag;
module_param(spi_hd_timing_diag, bool, 0644);
MODULE_PARM_DESC(spi_hd_timing_diag, "Collect aggregate transfer/control timing");
static unsigned long spi_hd_stat_rx_len_us;
module_param(spi_hd_stat_rx_len_us, ulong, 0444);
static unsigned long spi_hd_stat_rx_int1_us;
module_param(spi_hd_stat_rx_int1_us, ulong, 0444);
static unsigned long spi_hd_stat_rddma_us;
module_param(spi_hd_stat_rddma_us, ulong, 0444);
static unsigned long spi_hd_stat_rx_int0_us;
module_param(spi_hd_stat_rx_int0_us, ulong, 0444);
static uint spi_hd_stat_rx_frames;
module_param(spi_hd_stat_rx_frames, uint, 0444);
static unsigned long spi_hd_stat_wrdma_us;
module_param(spi_hd_stat_wrdma_us, ulong, 0444);
static unsigned long spi_hd_stat_wrend_us;
module_param(spi_hd_stat_wrend_us, ulong, 0444);
static unsigned long spi_hd_stat_credit_us;
module_param(spi_hd_stat_credit_us, ulong, 0444);

static uint spi_hd_sio_max_hz;
module_param(spi_hd_sio_max_hz, uint, 0644);
MODULE_PARM_DESC(spi_hd_sio_max_hz,
		 "Optional SIO clock cap; QIO bulk transfers retain the negotiated clock");

/* Isolated Pi5/C5 experiment: reduce ONLY enhanced H2E transfer SCLK. */
static uint spi_hd_qio_h2e_diag_max_hz;
module_param(spi_hd_qio_h2e_diag_max_hz, uint, 0644);
MODULE_PARM_DESC(spi_hd_qio_h2e_diag_max_hz,
		 "Diagnostic-only quad H2E SCLK cap in Hz (0 keeps negotiated clock)");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
static u32 spihd_logged_sclk_hz[2];
/* Keep independently changing H2E and E2H QIO diagnostic rates distinct. */
static u32 spihd_logged_h2e_qio_hz;
#endif

static bool spi_hd_wrdma_diag;
module_param(spi_hd_wrdma_diag, bool, 0644);
MODULE_PARM_DESC(spi_hd_wrdma_diag,
		 "Trace RX_BUF_LEN before/after WRDMA+WR_END for bring-up debugging");

/* Bound expensive diagnostic register reads to first QIO chunks only. */
static uint spi_hd_wrdma_diag_count;
#define SPIHD_WRDMA_DIAG_QUAD_LIMIT 12U

/*
 * Enhanced DesignWare WAIT_CYCLES are a read-side facility.  Keep the H2E
 * address single-lane by default so the required 8 write dummy clocks can be
 * emitted efficiently by extending the address phase (see
 * spihd_enhanced_xfer_lines()) rather than by inserting a software delay.
 */
/*
 * Generic controllers receive distinct command/address/dummy/data phases.
 * On RP1, the enhanced-write path needs the required 8 dummy clocks to be
 * folded into a second x1 address byte. This hardware-specific workaround
 * must be explicitly enabled (rpi_init.sh selects it for RP1).
 */
static bool spi_hd_qio_addr_single;
module_param(spi_hd_qio_addr_single, bool, 0644);
MODULE_PARM_DESC(spi_hd_qio_addr_single,
		 "RP1 workaround: fold 8 dummy clocks into x1 address for quad writes (default off)");

static bool spi_hd_skip_probe_reset;
module_param(spi_hd_skip_probe_reset, bool, 0644);
MODULE_PARM_DESC(spi_hd_skip_probe_reset,
		 "Skip GPIO reset during probe when the target was reset externally");

static bool spi_hd_force_mode0;
module_param(spi_hd_force_mode0, bool, 0644);
MODULE_PARM_DESC(spi_hd_force_mode0,
		 "Diagnostic: override DT CPOL/CPHA and run the host controller in SPI mode 0");

static uint spi_hd_credit_poll_us = SPIHD_CREDIT_POLL_US;
module_param(spi_hd_credit_poll_us, uint, 0644);
MODULE_PARM_DESC(spi_hd_credit_poll_us,
		 "Sleep between H2E RX-credit polls in microseconds (0 = re-read immediately)");

#define SPIHD_FAST_COALESCE     BIT(0)
#define SPIHD_FAST_CREDIT_ONCE  BIT(1)
#define SPIHD_SIO_HDR_MAX       3

static DEFINE_MUTEX(spihd_sio_lock);
static uint spi_hd_sio_fastpath;
module_param(spi_hd_sio_fastpath, uint, 0644);
MODULE_PARM_DESC(spi_hd_sio_fastpath,
		 "SIO overhead reduction bitmask: 1 = one transfer for cmd/addr/dummy(+write data), 2 = single plausible credit read");

static uint spi_hd_stat_segments;
module_param(spi_hd_stat_segments, uint, 0444);
MODULE_PARM_DESC(spi_hd_stat_segments, "H2E WRDMA+WR_END segments completed");

static uint spi_hd_stat_credit_reads;
module_param(spi_hd_stat_credit_reads, uint, 0444);
MODULE_PARM_DESC(spi_hd_stat_credit_reads, "RX_BUF_LEN credit register reads");

static uint spi_hd_stat_credit_sleeps;
module_param(spi_hd_stat_credit_sleeps, uint, 0444);
MODULE_PARM_DESC(spi_hd_stat_credit_sleeps, "Sleeps while waiting for an H2E RX credit");

static uint spi_hd_stat_resyncs;
module_param(spi_hd_stat_resyncs, uint, 0444);
MODULE_PARM_DESC(spi_hd_stat_resyncs, "Live-session transport resyncs completed");

static int spihd_inject_resync_set(const char *val, const struct kernel_param *kp);

static const struct kernel_param_ops spihd_inject_resync_ops = {
	.set = spihd_inject_resync_set,
};
module_param_cb(spi_hd_inject_resync, &spihd_inject_resync_ops, NULL, 0200);
MODULE_PARM_DESC(spi_hd_inject_resync,
		 "Diagnostic: write 1 to schedule a transport resync without a firmware reset");

#define SPIHD_AB_MAX_PHASES             8

static char *spi_hd_ab_phases;
module_param(spi_hd_ab_phases, charp, 0444);
MODULE_PARM_DESC(spi_hd_ab_phases,
		 "Diagnostic: timed phases after the post-boot clock, \"<credit poll us>[/<sio fastpath>]|inject:<seconds>,...\"");

struct spihd_ab_phase {
	int poll_us;            /* < 0: inject a transport resync */
	unsigned int fastpath;
	unsigned int secs;
};

static struct spihd_ab_phase spihd_ab[SPIHD_AB_MAX_PHASES];
static unsigned int spihd_ab_count;
static unsigned int spihd_ab_next;
static struct delayed_work spihd_ab_work;

static uint spi_hd_test_resync_ms[2];
static unsigned int spi_hd_test_resync_count;
static unsigned int spi_hd_test_resync_next;
module_param_array(spi_hd_test_resync_ms, uint, &spi_hd_test_resync_count, 0444);
MODULE_PARM_DESC(spi_hd_test_resync_ms,
		 "Opt-in raw test: delays in ms before up to two live resyncs");
static void spihd_test_resync_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(spihd_test_resync_work, spihd_test_resync_fn);

static bool spi_hd_driver_registered;
static DEFINE_MUTEX(spihd_bus_lock);

#define SPIHD_CONTROL_BYTES 64U

static size_t spihd_control_alignment(void)
{
	return max_t(size_t, SMP_CACHE_BYTES, dma_get_cache_alignment());
}

static size_t spihd_control_offset(void)
{
	return ALIGN((spi_hd_tx_batch ? SPI_HD_TX_BATCH_MAX : SPI_HD_BUF_SIZE) +
		     SPIHD_SIO_HDR_MAX, spihd_control_alignment());
}

static bool spihd_full_duplex(void)
{
	return !(SPIHD_CONTROLLER(spi_hd.spi)->flags & SPIHD_CTLR_HALF_DUPLEX);
}

static bool spihd_quad_available(struct spi_device *spi)
{
	return (spi->mode & (SPI_TX_QUAD | SPI_RX_QUAD)) ==
		(SPI_TX_QUAD | SPI_RX_QUAD);
}

/* Keep each protocol phase intact: generic transfer splitting cannot recreate
 * the ESP-HD command/address/dummy phases of an enhanced transaction.
 */
static int spihd_sync_transfer(struct spi_device *spi,
			       struct spi_transfer *xfer, unsigned int n)
{
	const void *original_tx[6] = {};
	void *original_rx[6] = {};
	size_t total = 0, tx_used = 0, rx_used = 0;
	size_t transfer_max = spi_max_transfer_size(spi);
	size_t message_max = spi_max_message_size(spi);
	u8 *control_tx = spi_hd.sio_buf + spihd_control_offset();
	u8 *control_rx = control_tx + ALIGN(SPIHD_CONTROL_BYTES, spihd_control_alignment());
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
		    (xfer[i].tx_buf && xfer[i].rx_buf && !spihd_full_duplex())) {
			dev_err_ratelimited(&spi->dev,
				"SPI-HD unsupported phase %u bpw=%u tx=%u rx=%u controller-flags=0x%x\n",
				i, xfer[i].bits_per_word, !!xfer[i].tx_buf,
				!!xfer[i].rx_buf, SPIHD_CONTROLLER(spi)->flags);
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
			if (xfer[i].len > SPIHD_CONTROL_BYTES - tx_used)
				return -EMSGSIZE;
			original_tx[i] = xfer[i].tx_buf;
			tx_used += xfer[i].len;
		}
		if (xfer[i].rx_buf && spihd_buf_is_on_stack(xfer[i].rx_buf)) {
			if (xfer[i].len > SPIHD_CONTROL_BYTES - rx_used)
				return -EMSGSIZE;
			original_rx[i] = xfer[i].rx_buf;
			rx_used += xfer[i].len;
		}
	}

	/* Control bytes previously lived on the kernel stack, which is not a
	 * DMA-safe SPI buffer. Reuse the staging allocation, with separate cache
	 * lines for TX and RX, without allocating in the transaction hot path.
	 */
	mutex_lock(&spihd_bus_lock);
	tx_used = rx_used = 0;
	for (i = 0; i < n; i++) {
		if (original_tx[i]) {
			memcpy(control_tx + tx_used, original_tx[i], xfer[i].len);
			xfer[i].tx_buf = control_tx + tx_used;
			tx_used += xfer[i].len;
		}
		if (original_rx[i]) {
			xfer[i].rx_buf = control_rx + rx_used;
			rx_used += xfer[i].len;
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

static bool spihd_get_cmd_info(struct sk_buff *skb, u8 *cmd_code,
			       u16 *cmd_seq);

static u32 spihd_effective_speed_hz(void)
{
	u32 requested_hz = (u32)spi_hd.spi_clk_mhz * NUMBER_1M;

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
	if (data_lines == 4 && h2e && spi_hd_qio_addr_single)
		return 1;

	return data_lines;
}

static u8 spihd_h2e_lines(void)
{
	return spi_hd_force_sio_h2e ? 1 : spi_hd.data_lines;
}

static u8 spihd_e2h_lines(void)
{
	return spi_hd_force_sio_e2h ? 1 : spi_hd.data_lines;
}

static u8 spihd_e2h_control_lines(void)
{
	return READ_ONCE(spi_hd_force_sio_control_e2h) ? 1 : spihd_e2h_lines();
}

static u32 spihd_get_le32(const u8 *buf)
{
	return (u32)buf[0] |
	       ((u32)buf[1] << 8) |
	       ((u32)buf[2] << 16) |
	       ((u32)buf[3] << 24);
}

static u32 spihd_diag_hash(const u8 *buf, size_t len)
{
	u32 hash = 2166136261U;

	while (len--) {
		hash ^= *buf++;
		hash *= 16777619U;
	}
	return hash;
}

static void spihd_put_le32(u8 *buf, u32 value)
{
	buf[0] = value;
	buf[1] = value >> 8;
	buf[2] = value >> 16;
	buf[3] = value >> 24;
}

static unsigned long spihd_gpio_irq_flags(struct gpio_desc *gpio)
{
	return gpiod_is_active_low(gpio) ?
		IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;
}

static struct sk_buff *spihd_alloc_skb(u32 len)
{
	struct sk_buff *skb;
	u32 alloc_len;
	u8 offset;

	alloc_len = max_t(u32, len, SPI_HD_BUF_SIZE) +
		    INTERFACE_HEADER_PADDING;
	skb = netdev_alloc_skb(NULL, alloc_len);
	if (!skb)
		return NULL;

	offset = ((unsigned long)skb->data) & (SKB_DATA_ADDR_ALIGNMENT - 1);
	if (offset)
		skb_reserve(skb, INTERFACE_HEADER_PADDING - offset);
	return skb;
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
			    spihd_get_cmd_info(skb, &cmd_code, &cmd_seq))
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

static bool spihd_combined_wrdma(size_t len, u8 lines)
{
	return READ_ONCE(spi_hd_wrdma_batch_end) && spi_hd_force_sio_wrend &&
	       spi_hd_qio_tx_word32 && spi_hd_qio_addr_single &&
	       lines == 4 && len > 256 && !(len & 3);
}

static bool spihd_combined_rddma(size_t len, u8 lines)
{
	return READ_ONCE(spi_hd_rddma_batch_end) &&
	       READ_ONCE(spi_hd_force_sio_control_e2h) &&
	       READ_ONCE(spi_hd_qio_rx_word32) &&
	       lines == 4 && len > 256 && !(len & 3);
}

static bool spihd_batch_rx_ack(size_t len, u8 lines)
{
	return READ_ONCE(spi_hd_rx_batch_control) &&
	       !(spi_hd_append_tx && spi_hd_append_no_ack) &&
	       spihd_combined_rddma(len, lines);
}

static bool spihd_rx_ack_required(u32 status, u32 pending)
{
	return !(spi_hd_append_tx && spi_hd_append_no_ack && !status &&
		 pending && pending <= spihd_rx_limit);
}

static int spihd_enhanced_xfer_lines(u8 base_cmd, bool has_addr, u8 addr,
				     const void *tx, void *rx, size_t len,
				     u8 lines)
{
	struct spi_transfer xfer[6] = {};
	bool h2e = !!tx;
	u8 addr_lines = spihd_addr_lines(lines, h2e);
	u8 cmd = spihd_wire_cmd_mode(base_cmd, lines, addr_lines);
	u8 addr_with_dummy[2] = { addr, 0 };
	u8 dummy = 0;
	u8 int1_hdr[SPIHD_SIO_HDR_MAX] = { SPIHD_CMD_INT1, 0, 0 };
	u8 end_hdr[SPIHD_SIO_HDR_MAX] = { base_cmd == SPIHD_CMD_RDDMA ? SPIHD_CMD_INT0 : SPIHD_CMD_WR_END, 0, 0 };
	unsigned int data_index = 0;
	bool fold_write_dummy;
	bool word32 = false;
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
	if (lines == 1 && READ_ONCE(spi_hd_sio_max_hz))
		speed = min(speed, READ_ONCE(spi_hd_sio_max_hz));
	/* Leave E2H QIO, SIO control and WR_END on their original clocks. */
	if (tx && lines == 4 && READ_ONCE(spi_hd_qio_h2e_diag_max_hz))
		speed = min(speed, READ_ONCE(spi_hd_qio_h2e_diag_max_hz));
	data_nbits = spihd_data_nbits_lines(lines);
	addr_nbits = spihd_data_nbits_lines(addr_lines);

	/*
	 * In SIO the cmd/addr/dummy bytes are plain x1 bytes on the wire, so one
	 * controller transfer carries them (and write data) instead of three or
	 * four separately programmed ones. The wire bits are identical.
	 */
	if ((READ_ONCE(spi_hd_sio_fastpath) & SPIHD_FAST_COALESCE) &&
	    lines == 1 && addr_lines == 1 && spi_hd.sio_buf &&
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
			 * Register reads stay below the controller DMA threshold.
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
	 * DW SSI WAIT_CYCLES are generated for enhanced reads, not writes.
	 * ESP Slave-HD nevertheless requires 8 dummy clocks between address and
	 * write data.  With QIO data + x1 address, represent those clocks as an
	 * extra zero address byte.  Electrically the C5 sees exactly the required
	 * 8-bit address followed by 8 idle clocks before the x4 data phase.
	 *
	 * This is controller framing, not a delay: it costs only the protocol's
	 * required clocks and therefore scales cleanly to the 24/40 MHz target.
	 */
	fold_write_dummy = tx && lines == 4 && addr_lines == 1 && has_addr;

	if (base_cmd == SPIHD_CMD_RDDMA && spihd_batch_rx_ack(len, lines)) {
		u32 control_speed = speed;

		if (READ_ONCE(spi_hd_sio_max_hz))
			control_speed = min(control_speed, READ_ONCE(spi_hd_sio_max_hz));
		xfer[n].tx_buf = int1_hdr;
		xfer[n].len = sizeof(int1_hdr);
		xfer[n].tx_nbits = SPI_NBITS_SINGLE;
		xfer[n].bits_per_word = 8;
		xfer[n].speed_hz = control_speed;
		xfer[n].cs_change = 1;
		SPIHD_CS_CHANGE_NO_DELAY(&xfer[n]);
		n++;
	}

	xfer[n].tx_buf = &cmd;
	xfer[n].len = 1;
	xfer[n].tx_nbits = SPI_NBITS_SINGLE;
	xfer[n].bits_per_word = 8;
	xfer[n].speed_hz = speed;
	n++;

	if (has_addr) {
		xfer[n].tx_buf = fold_write_dummy ? addr_with_dummy : &addr;
		xfer[n].len = fold_write_dummy ? sizeof(addr_with_dummy) : 1;
		xfer[n].tx_nbits = addr_nbits;
		xfer[n].bits_per_word = 8;
		xfer[n].speed_hz = speed;
		n++;
	}

	if (!fold_write_dummy) {
		xfer[n].tx_buf = &dummy;
		xfer[n].len = 1;
		xfer[n].tx_nbits = SPI_NBITS_SINGLE;
		xfer[n].bits_per_word = 8;
		xfer[n].speed_hz = speed;
		SPIHD_SET_DUMMY(&xfer[n]);
		n++;
	}

	if (tx) {
		xfer[n].tx_buf = tx;
		xfer[n].tx_nbits = data_nbits;
		if (READ_ONCE(spi_hd_qio_tx_word32) && lines == 4 && len > 256 &&
		    !(len & 3) &&
		    len <= (spi_hd_tx_batch ? SPI_HD_TX_BATCH_MAX : SPI_HD_BUF_SIZE) &&
		    spi_hd.sio_buf &&
		    spihd_bpw_supported(spi_hd.spi, 32)) {
			const u8 *src = tx;
			u32 *dst = (u32 *)spi_hd.sio_buf;
			size_t i;

			/* Word32 is an optimization, not a requirement: use
			 * native 8-bit quad DMA for arbitrary Ethernet lengths.
			 * No new staging allocation or transport retries.
			 */
			/* Native FIFO words shift MSB first. Pack explicitly so the
			 * DMA's native-endian words retain the original wire order.
			 * Reuse the aligned staging buffer under its existing lock.
			 */
			mutex_lock(&spihd_sio_lock);
			for (i = 0; i < len; i += 4)
				dst[i / 4] = (u32)src[i] << 24 |
					     (u32)src[i + 1] << 16 |
					     (u32)src[i + 2] << 8 | src[i + 3];
			xfer[n].tx_buf = dst;
			word32 = true;
		}
	} else {
		xfer[n].rx_buf = rx;
		xfer[n].rx_nbits = data_nbits;
		if (READ_ONCE(spi_hd_qio_rx_word32) && lines == 4 && len > 256 &&
		    !(len & 3) && spihd_bpw_supported(spi_hd.spi, 32)) {
			word32 = true;
		}
	}
	xfer[n].len = len;
	xfer[n].bits_per_word = word32 ? 32 : 8;
	xfer[n].speed_hz = speed;
	data_index = n++;
	if ((base_cmd == SPIHD_CMD_WRDMA && spihd_combined_wrdma(len, lines)) ||
	    (base_cmd == SPIHD_CMD_RDDMA && !spi_hd_append_tx &&
	     spihd_combined_rddma(len, lines))) {
		u32 end_speed = spihd_effective_speed_hz();

		if (READ_ONCE(spi_hd_sio_max_hz))
			end_speed = min(end_speed, READ_ONCE(spi_hd_sio_max_hz));
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
	if (word32 && tx)
		mutex_unlock(&spihd_sio_lock);
	if (!ret && word32 && rx) {
		u8 *dst = rx;
		size_t i;

		for (i = 0; i < len; i += 4) {
			u32 word;

			memcpy(&word, dst + i, sizeof(word));
			dst[i] = word >> 24;
			dst[i + 1] = word >> 16;
			dst[i + 2] = word >> 8;
			dst[i + 3] = word;
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

static int spihd_command_lines(u8 base_cmd, u8 lines)
{
	struct spi_transfer xfer[3] = {};
	u8 addr_lines = spihd_addr_lines(lines, true);
	u8 cmd = spihd_wire_cmd_mode(base_cmd, lines, addr_lines);
	u8 addr = 0;
	u8 dummy = 0;
	u32 speed;
	u8 addr_nbits;

	if (!spi_hd.spi)
		return -ENODEV;

	/*
	 * ESP-IDF configures SPI Slave HD with 8-bit command, address and dummy
	 * phases for every transaction. ESSL keeps address=0 and the 8 dummy
	 * clocks even for zero-data WR_END/INTx commands.
	 */
	speed = spihd_effective_speed_hz();
	if (lines == 1 && READ_ONCE(spi_hd_sio_max_hz))
		speed = min(speed, READ_ONCE(spi_hd_sio_max_hz));
	addr_nbits = spihd_data_nbits_lines(addr_lines);

	if ((READ_ONCE(spi_hd_sio_fastpath) & SPIHD_FAST_COALESCE) &&
	    lines == 1 && addr_lines == 1) {
		u8 hdr[SPIHD_SIO_HDR_MAX] = { cmd, 0, 0 };

		xfer[0].tx_buf = hdr;
		xfer[0].len = sizeof(hdr);
		xfer[0].tx_nbits = SPI_NBITS_SINGLE;
		xfer[0].bits_per_word = 8;
		xfer[0].speed_hz = speed;
		return spihd_sync_transfer(spi_hd.spi, xfer, 1);
	}

	xfer[0].tx_buf = &cmd;
	xfer[0].len = 1;
	xfer[0].tx_nbits = SPI_NBITS_SINGLE;
	xfer[0].bits_per_word = 8;
	xfer[0].speed_hz = speed;

	xfer[1].tx_buf = &addr;
	xfer[1].len = 1;
	xfer[1].tx_nbits = addr_nbits;
	xfer[1].bits_per_word = 8;
	xfer[1].speed_hz = speed;

	xfer[2].tx_buf = &dummy;
	xfer[2].len = 1;
	xfer[2].tx_nbits = SPI_NBITS_SINGLE;
	xfer[2].bits_per_word = 8;
	xfer[2].speed_hz = speed;
	SPIHD_SET_DUMMY(&xfer[2]);

	return spihd_sync_transfer(spi_hd.spi, xfer, ARRAY_SIZE(xfer));
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

	if (READ_ONCE(spi_hd_sio_max_hz))
		speed = min(speed, READ_ONCE(spi_hd_sio_max_hz));
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
	return spihd_full_duplex() && spi_hd_append_tx && spi_hd_tx_len_mirror &&
	       spihd_tx_len_mirror_available && spihd_e2h_control_lines() == 1 &&
	       (READ_ONCE(spi_hd_sio_fastpath) & SPIHD_FAST_COALESCE) &&
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

	if (READ_ONCE(spi_hd_sio_max_hz))
		speed = min(speed, READ_ONCE(spi_hd_sio_max_hz));
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
	return spihd_read_reg_stable_lines(addr, value, spihd_e2h_control_lines());
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
	return spihd_write_reg_lines(addr, value, spihd_h2e_lines());
}

static int spihd_rddma(void *buf, size_t len)
{
	u8 lines = READ_ONCE(spi_hd_force_sio_control_e2h) && len <= 256 ?
		   1 : spihd_e2h_lines();
	bool timing = READ_ONCE(spi_hd_timing_diag);
	u64 started = 0;
	int ret;

	/*
	 * ESP SPI-HD configures an 8-bit address phase for every data command.
	 * The DMA commands use address 0, matching esp-hosted-mcu's master
	 * implementation where .address_bits = 8 and .addr defaults to zero.
	 * Keeping this phase is also what selects RP1 DesignWare TT1:
	 * instruction x1, address x4, data x4.
	 */
	spi_hd.rx_inflight = true;
	if (timing)
		started = ktime_get_ns();
	ret = spihd_enhanced_xfer_lines(SPIHD_CMD_RDDMA, true, 0,
					 NULL, buf, len, lines);
	if (timing)
		spi_hd_stat_rddma_us += div_u64(ktime_get_ns() - started, 1000);
	if (ret) {
		if (ret == -EINVAL || ret == -EOPNOTSUPP ||
		    ret == -EMSGSIZE || ret == -ERANGE)
			spi_hd.rx_inflight = false;
		return ret;
	}

	if (!spi_hd_append_tx && !spihd_combined_rddma(len, lines)) {
		if (timing)
			started = ktime_get_ns();
		ret = spihd_command_lines(SPIHD_CMD_INT0, spihd_e2h_control_lines());
		if (timing)
			spi_hd_stat_rx_int0_us += div_u64(ktime_get_ns() - started, 1000);
	}
	if (!ret)
		spi_hd.rx_inflight = false;
	return ret;
}

static int spihd_wrdma_segment(const void *buf, size_t len, u8 wr_lines)
{
	bool diag = READ_ONCE(spi_hd_wrdma_diag) && wr_lines == 4 &&
		    spi_hd_wrdma_diag_count < SPIHD_WRDMA_DIAG_QUAD_LIMIT;
	u8 end_lines = spi_hd_force_sio_wrend ? 1 : wr_lines;
	u32 before = 0, after = 0, c5_rx_bits = 0, c5_dma_raw = 0;
	u32 c5_dma_len = 0;
	int before_ret = 0, after_ret = 0, bits_ret = 0, raw_ret = 0;
	int len_ret = 0;
	u64 started = 0;
	bool timing = READ_ONCE(spi_hd_timing_diag);
	int ret;

	if (diag) {
		spi_hd_wrdma_diag_count++;
		esp_info("SPI-HD QIO TX bytes: idx=%u len=%zu data=%*ph\n",
			 spi_hd_wrdma_diag_count, len,
			 (int)min_t(size_t, len, 48), buf);
		before_ret = spihd_read_reg_stable(SPIHD_REG_RX_BUF_LEN, &before);
	}

	spi_hd.tx_inflight = true;
	if (timing)
		started = ktime_get_ns();
	ret = spihd_enhanced_xfer_lines(SPIHD_CMD_WRDMA, true, 0,
					 buf, NULL, len, wr_lines);
	if (timing)
		spi_hd_stat_wrdma_us += div_u64(ktime_get_ns() - started, 1000);
	if (ret) {
		/* Rejected by the controller before anything was clocked out. */
		if (ret == -EINVAL || ret == -EOPNOTSUPP ||
		    ret == -EMSGSIZE || ret == -ERANGE)
			spi_hd.tx_inflight = false;
		esp_err("SPI-HD WRDMA payload failure: len=%zu lines=%u ret=%d\n",
			len, wr_lines, ret);
		return ret;
	}

	if (!spihd_combined_wrdma(len, wr_lines)) {
		if (timing)
			started = ktime_get_ns();
		ret = spihd_command_lines(SPIHD_CMD_WR_END, end_lines);
		if (timing)
			spi_hd_stat_wrend_us += div_u64(ktime_get_ns() - started, 1000);
	}
	if (ret) {
		esp_err("SPI-HD WR_END failure: len=%zu lines=%u ret=%d\n",
			len, end_lines, ret);
		return ret;
	}
	spi_hd.tx_inflight = false;
	spi_hd_stat_segments++;

	if (diag) {
		after_ret = spihd_read_reg_stable(SPIHD_REG_RX_BUF_LEN, &after);
		esp_info("SPI-HD WRDMA diag: len=%zu hash=0x%08x wr_lines=%u end_lines=%u before_ret=%d before=0x%08x after_ret=%d after=0x%08x local_segments=%u\n",
			 len, spihd_diag_hash(buf, len), wr_lines, end_lines,
			 before_ret, before, after_ret, after,
			 spi_hd.tx_segment_count);
		/*
		 * C5 callback snapshots from the separate shared-buffer
		 * diagnostic registers; SIO read avoids coupling this probe
		 * to the broken enhanced-QIO host TX path.
		 */
		bits_ret = spihd_read_reg_stable_lines(
			SPIHD_REG_RX_DIAG_BITS, &c5_rx_bits, 1);
		raw_ret = spihd_read_reg_stable_lines(
			SPIHD_REG_RX_DIAG_RAW, &c5_dma_raw, 1);
		esp_info("SPI-HD C5 HW RX diag: bits_ret=%d bits=%u raw_ret=%d dma_raw=0x%08x\n",
			 bits_ret, c5_rx_bits, raw_ret, c5_dma_raw);
		len_ret = spihd_read_reg_stable_lines(
			SPIHD_REG_RX_DIAG_LEN, &c5_dma_len, 1);
		esp_info("SPI-HD C5 RX descriptor diag: read_ret=%d seq=%u trans_len=%u\n",
			 len_ret, c5_dma_len >> 16, c5_dma_len & 0xffffU);
	}

	return 0;
}

static int spihd_wait_slave_ready(void)
{
	unsigned int waited = 0;
	u32 value = 0;
	u32 last_value = 0;
	int last_ret = -ETIMEDOUT;
	int ret;

	spihd_tx_len_mirror_available = false;
	while (waited < SPIHD_READY_TIMEOUT_MS) {
		ret = spihd_read_reg_stable(SPIHD_REG_SLAVE_READY, &value);
		last_ret = ret;
		if (!ret) {
			last_value = value;
			if ((value & 0xff) == SPIHD_STATE_SLAVE_READY) {
				spihd_tx_len_mirror_available = !!(value & SPIHD_CAP_TX_LEN_MIRROR);
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

static int spihd_open_slave_datapath(void)
{
	return spihd_write_reg(SPIHD_REG_SLAVE_CTRL,
				SPIHD_CTRL_DATAPATH_ON);
}

static int spihd_wait_tx_credit(void)
{
	u32 status_word = 0;
	s32 free_slots = 0;
	u32 last_status = 0;
	int last_ret = 0;
	unsigned long deadline;
	unsigned int poll_us;
	int ret;

	/*
	 * The ESP publishes a cumulative RX slot count when DMA descriptors
	 * are armed. One worker serializes WRDMA/WR_END, so already-observed
	 * credits remain valid until consumed; don't issue two synchronous
	 * stable-register reads on every ~60-byte segment.
	 *
	 * Reset this snapshot on every FW-generation reset/recovery.
	 */
	if ((s32)(spi_hd.rx_credit_limit - spi_hd.tx_segment_count) > 0)
		return 0;

	deadline = jiffies + msecs_to_jiffies(SPIHD_CREDIT_TIMEOUT_MS);
	for (;;) {
		/*
		 * A torn read of the cumulative counter mixes old/new bytes and
		 * lands >= 256 away or behind, so a small positive delta is a
		 * real credit; anything else takes the stable double read.
		 */
		if (READ_ONCE(spi_hd_sio_fastpath) & SPIHD_FAST_CREDIT_ONCE) {
			u8 raw[4];

			spi_hd_stat_credit_reads++;
			if (!spihd_read_reg_lines(SPIHD_REG_RX_BUF_LEN, raw,
						  sizeof(raw), spihd_e2h_control_lines())) {
				status_word = spihd_get_le32(raw);
				free_slots = (s32)(status_word - spi_hd.tx_segment_count);
				if (free_slots > 0 && free_slots <= SPIHD_LIVE_CREDIT_MAX) {
					spi_hd.rx_credit_limit = status_word;
					return 0;
				}
				last_ret = 0;
				last_status = status_word;
				if (free_slots <= 0)
					goto next_poll;
			}
		}
		ret = spihd_read_reg_stable(SPIHD_REG_RX_BUF_LEN, &status_word);
		spi_hd_stat_credit_reads++;
		last_ret = ret;
		if (!ret) {
			last_status = status_word;
			/*
			 * The firmware and host counters wrap modulo 2^32.
			 * Treat a stale/behind credit count as NOT READY, rather
			 * than allowing u32 underflow to masquerade as free slots.
			 */
			free_slots = (s32)(status_word - spi_hd.tx_segment_count);
			/* A slave advertises at most its bounded RX descriptor window.
			 * Never accept an impossible positive delta from a stale or
			 * corrupt cumulative register, even after a stable reread.
			 */
			if (free_slots > 0 && free_slots <= SPIHD_LIVE_CREDIT_MAX) {
				spi_hd.rx_credit_limit = status_word;
				return 0;
			}
		}
next_poll:
		if (time_after(jiffies, deadline))
			break;
		poll_us = READ_ONCE(spi_hd_credit_poll_us);
		if (poll_us) {
			spi_hd_stat_credit_sleeps++;
			usleep_range(poll_us, poll_us + poll_us / 2);
		} else {
			cond_resched();
		}
	}

	esp_err("SPI-HD TX credit timeout: lines=%u read_ret=%d RX_BUF_LEN=0x%08x tx_segments=%u free=%d\n",
		spi_hd.data_lines, last_ret, last_status,
		spi_hd.tx_segment_count, free_slots);
	return -ETIMEDOUT;
}

static bool spihd_get_cmd_info(struct sk_buff *skb, u8 *cmd_code,
			       u16 *cmd_seq)
{
	struct esp_payload_header *header;
	struct command_header *cmd;
	u16 offset;

	if (!skb || skb->len < sizeof(*header))
		return false;

	header = (struct esp_payload_header *)skb->data;
	if (header->packet_type != PACKET_TYPE_COMMAND_REQUEST)
		return false;

	offset = esp_wire_le16_to_cpu(header->offset);
	if (offset + sizeof(*cmd) > skb->len)
		return false;

	cmd = (struct command_header *)(skb->data + offset);
	if (cmd_code)
		*cmd_code = cmd->cmd_code;
	if (cmd_seq)
		*cmd_seq = esp_wire_le16_to_cpu(cmd->seq_num);
	return true;
}

static int spihd_send_frame(struct sk_buff *skb)
{
	size_t offset = 0;
	size_t segment;
	u8 wr_lines;
	int ret;

	if (!skb || !skb->data || !skb->len)
		return -EINVAL;

	if (skb->len > (spi_hd_tx_batch ? SPI_HD_TX_BATCH_MAX : SPI_HD_BUF_SIZE))
		return -EMSGSIZE;

	wr_lines = spihd_h2e_lines();
	if (skb->len >= sizeof(struct esp_payload_header)) {
		const struct esp_payload_header *header =
			(const struct esp_payload_header *)skb->data;
		/*
		 * Never depend on enhanced quad PIO for short Hosted commands:
		 * RP1/C5 can acknowledge the WRDMA yet receive zero-filled
		 * payloads for e.g. the 31-byte INIT_INTERFACE command.
		 * Keep command/EAPOL/event/service traffic on the proven SIO
		 * control path. Route eligible Wi-Fi data and raw test payloads
		 * through negotiated QIO only once the DMA-friendly length is
		 * available. This does not add any retransmission or memory.
		 */
		if (header->if_type != ESP_TEST_IF &&
		    (READ_ONCE(spi_hd_force_sio_control_h2e) ||
		     header->packet_type != PACKET_TYPE_DATA ||
		     skb->len < 128)) {
			wr_lines = 1;
			pr_info_once("SPI-HD: Hosted control/short payloads SIO; eligible Wi-Fi data QIO\n");
		}
	}

	if (spi_hd_wrdma_diag && spi_hd_wrdma_diag_count < SPIHD_WRDMA_DIAG_QUAD_LIMIT)
		esp_info("SPI-HD H2E frame diag: len=%u hash=0x%08x\n",
			 skb->len, spihd_diag_hash(skb->data, skb->len));

	segment = clamp_t(size_t, spi_hd_tx_segment,
			  SPI_HD_TX_SEGMENT_MIN,
			  spi_hd.tx_segment_max ? spi_hd.tx_segment_max :
			  SPI_HD_TX_SEGMENT_DEFAULT);

	while (offset < skb->len) {
		size_t chunk = min(segment, skb->len - offset);
		u64 started = 0;
		bool timing = READ_ONCE(spi_hd_timing_diag);

		if (timing)
			started = ktime_get_ns();
		ret = spihd_wait_tx_credit();
		if (timing)
			spi_hd_stat_credit_us += div_u64(ktime_get_ns() - started, 1000);
		if (ret)
			return ret;

		ret = spihd_wrdma_segment(skb->data + offset, chunk, wr_lines);
		/*
		 * A controller error does not guarantee that zero clocks were
		 * emitted. Never resend a failed segment based on errno alone:
		 * the peripheral might already have consumed part of the frame.
		 * Report failure and let transport reset/recovery re-synchronize.
		 */
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
		/* Legacy boot/control packets can be a sole unpadded frame. */
		span = frame_len == total - pos ? frame_len : ALIGN(frame_len, 4);
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
	else if (header->if_type == ESP_HCI_IF)
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
		unsigned int dump = min_t(unsigned int, skb->len, 32);

		esp_err("SPI-HD malformed RX batch: bytes=%u consumed=0x%06x limit=%u append=%u\n",
			skb->len, spi_hd.rx_byte_count, spihd_rx_limit, spi_hd_append_tx);
		esp_verbose("SPI-HD malformed RX batch: bytes=%u prefix=%*ph suffix=%*ph\n",
			skb->len, dump, skb->data, dump,
			skb->data + skb->len - dump);
		if (skb->len >= 1472 + 32)
			esp_verbose("SPI-HD malformed RX raw boundary: offset=1472 data=%32ph\n",
				skb->data + 1472);
		if (spi_hd_append_tx)
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
		size_t span = len == skb->len - pos ? len : ALIGN(len, 4);

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
	if (spihd_rx_batch_logs++ < 3)
		esp_info("SPI-HD RX batch: frames=%u bytes=%u\n", frames, skb->len);
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
	bool timing = READ_ONCE(spi_hd_timing_diag);
	u64 started = 0;
	int ret;

	if (!spi_hd.spi)
		return -ENODEV;

	if (timing)
		started = ktime_get_ns();
	if (spihd_use_tx_len_mirror())
		ret = spihd_read_tx_len_mirrored(&status_word);
	else if (spihd_full_duplex() && READ_ONCE(spi_hd_rx_batch_control) &&
	    spihd_e2h_control_lines() == 1 &&
	    (READ_ONCE(spi_hd_sio_fastpath) & SPIHD_FAST_COALESCE) &&
	    spi_max_transfer_size(spi_hd.spi) >= 7 &&
	    spi_max_message_size(spi_hd.spi) >= 14)
		ret = spihd_read_tx_len_batched(&status_word);
	else
		ret = spihd_read_reg_stable(SPIHD_REG_TX_BUF_LEN, &status_word);
	if (timing)
		spi_hd_stat_rx_len_us += div_u64(ktime_get_ns() - started, 1000);
	if (ret)
		return ret;

	status = status_word & SPIHD_INT_MASK;
	status_word &= SPIHD_TX_BUF_LEN_MASK;
	pending = (status_word - spi_hd.rx_byte_count) & SPIHD_TX_BUF_LEN_MASK;
	/* Append DMA completion clears DataReady; status and idle still ACK. */
	if (spihd_rx_ack_required(status, pending) &&
	    !(spihd_batch_rx_ack(pending, spihd_e2h_lines()) &&
	      pending <= spihd_rx_limit)) {
		if (timing)
			started = ktime_get_ns();
		ret = spihd_command_lines(SPIHD_CMD_INT1, spihd_e2h_control_lines());
		if (timing)
			spi_hd_stat_rx_int1_us += div_u64(ktime_get_ns() - started, 1000);
		if (ret)
			return ret;
	}
	if (!pending)
		return status ? 0 : -EAGAIN;
	if (pending > spihd_rx_limit) {
		esp_err("SPI-HD RX length %u exceeds %u\n",
			pending, spihd_rx_limit);
		return -EMSGSIZE;
	}

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
	spi_hd_stat_rx_frames++;

	/*
	 * Match the upstream SPI-HD bring-up sequence: register access and the
	 * capability/boot frame use one data line. Once that boot frame is in
	 * hand, widen subsequent traffic to QIO. The ESP slave command decoder
	 * accepts the line mode encoded in each command byte.
	 */
	if (pending >= sizeof(struct esp_payload_header)) {
		struct esp_payload_header *header =
			(struct esp_payload_header *)skb->data;
		u16 frame_offset = esp_wire_le16_to_cpu(header->offset);

		if (header->if_type == ESP_INTERNAL_IF &&
		    frame_offset + sizeof(struct event_header) <= pending) {
			struct event_header *event =
				(struct event_header *)(skb->data + frame_offset);

			if (event->event_code == ESP_INTERNAL_BOOTUP_EVENT &&
			    spi_hd.data_lines != 4) {
				if (spi_hd_force_sio || !spihd_quad_available(spi_hd.spi)) {
					esp_info("SPI-HD boot event received; using SIO (forced or no bidirectional quad wiring)\n");
				} else {
					u32 ctrl_verify = 0;
					u32 scratch_verify = 0;
					u32 scratch_cross = 0;
					const u32 scratch_pattern = 0xa55ac33c;
					const u8 scratch_long_pattern[20] = {
						0x00, 0x00, 0x01, 0x00,
						0x08, 0x00, 0x0c, 0x00,
						0x17, 0x00, 0x00, 0x00,
						0x01, 0x00, 0x00, 0x00,
						0x01, 0x00, 0x00, 0x00,
					};
					const u8 scratch_long_sentinel[20] = {
						0xd0, 0xd1, 0xd2, 0xd3,
						0xd4, 0xd5, 0xd6, 0xd7,
						0xd8, 0xd9, 0xda, 0xdb,
						0xdc, 0xdd, 0xde, 0xdf,
						0xe0, 0xe1, 0xe2, 0xe3,
					};
					u8 scratch_long_verify[sizeof(scratch_long_pattern)] = {};
					int qio_wr_ret;
					int qio_rd_ret;
					int scratch_prefill_ret = 0;
					int scratch_wr_ret = 0;
					int scratch_rd_ret = 0;
					int cross_wr_ret = 0;
					int cross_rd_ret = 0;
					int long_prefill_wr_ret = 0;
					int long_prefill_rd_ret = 0;
					int long_wr_ret = 0;
					int long_rd_ret = 0;

					spi_hd.data_lines = 4;
					esp_info("SPI-HD bring-up complete; switching data/address phases to QIO\n");

					/*
					 * Safe one-time QIO write/read probe. Rewriting
					 * DATAPATH_ON is idempotent and lets bring-up
					 * distinguish a generic RP1 quad-TX problem from
					 * a WRDMA-specific problem before the first
					 * command frame is sent.
					 */
					qio_wr_ret = spihd_write_reg(SPIHD_REG_SLAVE_CTRL,
								   SPIHD_CTRL_DATAPATH_ON);
					qio_rd_ret = spihd_read_reg_stable(
						SPIHD_REG_SLAVE_CTRL, &ctrl_verify);
					esp_info("SPI-HD QIO WRBUF selftest: write=%d read=%d SLAVE_CTRL=0x%08x\n",
						 qio_wr_ret, qio_rd_ret, ctrl_verify);
					/* A controller can advertise quad but fail its enhanced
					 * transaction. Keep the operational path in SIO if
					 * QIO initialization fails.
					 */
					if (qio_wr_ret || qio_rd_ret ||
					    !(ctrl_verify & SPIHD_CTRL_DATAPATH_ON)) {
						esp_warn("SPI-HD QIO boot selftest failed; using SIO\n");
						spi_hd.data_lines = 1;
					}

					/*
					 * DATAPATH_ON=1 barely exercises the four QIO
					 * lanes.  Under WRDMA diagnostics also write a
					 * high-transition pattern to an otherwise unused
					 * shared-register word and read it back.  This
					 * proves the actual QIO data phase/lane mapping,
					 * not merely command decoding.
					 */
					/* Keep forced-SIO RX pilots out of unrelated QIO RX selftests. */
					if (spi_hd.data_lines == 4 && spi_hd_wrdma_diag && !spi_hd_force_sio_e2h) {
						/*
						 * C5 exposes a 64-byte Slave-HD shared buffer,
						 * therefore 0x20..0x23 is a valid scratch word.
						 * Cross the line modes to isolate quad TX from
						 * quad RX rather than testing both at once.
						 */
						cross_wr_ret = spihd_write_reg_lines(
							SPIHD_REG_QIO_SCRATCH,
							scratch_pattern, 1);
						cross_rd_ret = spihd_read_reg_stable_lines(
							SPIHD_REG_QIO_SCRATCH,
							&scratch_cross, 4);
						esp_info("SPI-HD QIO RDBUF crosscheck: SIO-write=%d QIO-read=%d wrote=0x%08x readback=0x%08x %s\n",
							 cross_wr_ret, cross_rd_ret,
							 scratch_pattern, scratch_cross,
							 (!cross_wr_ret && !cross_rd_ret &&
							  scratch_cross == scratch_pattern) ?
							 "PASS" : "FAIL");

						/* Always leave a different SIO value before a QIO write. */
						scratch_prefill_ret = spihd_write_reg_lines(
							SPIHD_REG_QIO_SCRATCH,
							~scratch_pattern, 1);
						scratch_verify = 0;
						scratch_wr_ret = spihd_write_reg_lines(
							SPIHD_REG_QIO_SCRATCH,
							scratch_pattern, 4);
						scratch_rd_ret = spihd_read_reg_stable_lines(
							SPIHD_REG_QIO_SCRATCH,
							&scratch_verify, 1);
						esp_info("SPI-HD QIO WRBUF crosscheck: QIO-write=%d SIO-read=%d wrote=0x%08x readback=0x%08x %s\n",
							 scratch_wr_ret, scratch_rd_ret,
							 scratch_pattern, scratch_verify,
							 (!scratch_prefill_ret && !scratch_wr_ret &&
							  !scratch_rd_ret &&
							  scratch_verify == scratch_pattern) ?
							 "PASS" : "FAIL");

						/* Always leave a different SIO value before a QIO write. */
						scratch_prefill_ret = spihd_write_reg_lines(
							SPIHD_REG_QIO_SCRATCH,
							~scratch_pattern, 1);
						scratch_verify = 0;
						scratch_wr_ret = spihd_write_reg_lines(
							SPIHD_REG_QIO_SCRATCH,
							scratch_pattern, 4);
						scratch_rd_ret = spihd_read_reg_stable_lines(
							SPIHD_REG_QIO_SCRATCH,
							&scratch_verify, 4);
						esp_info("SPI-HD QIO WRBUF pattern: write=%d read=%d wrote=0x%08x readback=0x%08x %s\n",
							 scratch_wr_ret, scratch_rd_ret,
							 scratch_pattern, scratch_verify,
							 (!scratch_prefill_ret && !scratch_wr_ret &&
							  !scratch_rd_ret &&
							  scratch_verify == scratch_pattern) ?
							 "PASS" : "FAIL");

						/*
						 * Match the failing first Hosted H2E frame length and
						 * bytes, but use WRBUF instead of WRDMA. Read it back
						 * over SIO so this isolates RP1's 20-byte QIO TX path
						 * from C5 WRDMA/GDMA semantics.
						 */
						/*
						 * First prove a long WRBUF works over SIO and leave a
						 * recognizable 20-byte sentinel in the shared buffer.
						 * If the following QIO write is partial, the untouched
						 * suffix exposes exactly how many bytes were committed.
						 */
						long_prefill_wr_ret = spihd_enhanced_xfer_lines(
							SPIHD_CMD_WRBUF, true,
							SPIHD_REG_QIO_SCRATCH,
							scratch_long_sentinel, NULL,
							sizeof(scratch_long_sentinel), 1);
						long_prefill_rd_ret = spihd_enhanced_xfer_lines(
							SPIHD_CMD_RDBUF, true,
							SPIHD_REG_QIO_SCRATCH, NULL,
							scratch_long_verify,
							sizeof(scratch_long_verify), 1);
						esp_info("SPI-HD SIO WRBUF 20B baseline: write=%d read=%d rx=%*ph %s\n",
							 long_prefill_wr_ret, long_prefill_rd_ret,
							 (int)sizeof(scratch_long_verify),
							 scratch_long_verify,
							 (!long_prefill_wr_ret && !long_prefill_rd_ret &&
							  !memcmp(scratch_long_sentinel,
								  scratch_long_verify,
								  sizeof(scratch_long_sentinel))) ?
							 "PASS" : "FAIL");

						/*
						 * Read the same 20-byte sentinel over QIO before writing
						 * anything via QIO.  This independently distinguishes
						 * long quad RX framing from the problematic quad TX path.
						 */
						memset(scratch_long_verify, 0,
						       sizeof(scratch_long_verify));
						long_rd_ret = spihd_enhanced_xfer_lines(
							SPIHD_CMD_RDBUF, true,
							SPIHD_REG_QIO_SCRATCH, NULL,
							scratch_long_verify,
							sizeof(scratch_long_verify), 4);
						esp_info("SPI-HD QIO RDBUF 20B isolation: read=%d rx=%*ph %s\n",
							 long_rd_ret,
							 (int)sizeof(scratch_long_verify),
							 scratch_long_verify,
							 (!long_rd_ret &&
							  !memcmp(scratch_long_sentinel,
								  scratch_long_verify,
								  sizeof(scratch_long_sentinel))) ?
							 "PASS" : "FAIL");
						/*
						 * Map the exact RP1 enhanced-QIO write-length boundary.
						 * Each case restores the 20-byte SIO sentinel first, then
						 * writes only the requested QIO prefix and reads the full
						 * region back over SIO.  This distinguishes a fixed
						 * controller cutoff from NDF/length-dependent behavior.
						 */
						{
							static const u8 qio_len_cases[] = {
								5, 8, 9, 10, 12, 16,
							};
							u8 expected[sizeof(scratch_long_pattern)];
							size_t case_idx;

							for (case_idx = 0;
							     case_idx < ARRAY_SIZE(qio_len_cases);
							     case_idx++) {
								u8 qio_len = qio_len_cases[case_idx];

								spihd_enhanced_xfer_lines(
									SPIHD_CMD_WRBUF, true,
									SPIHD_REG_QIO_SCRATCH,
									scratch_long_sentinel, NULL,
									sizeof(scratch_long_sentinel), 1);
								memset(scratch_long_verify, 0,
								       sizeof(scratch_long_verify));
								memcpy(expected, scratch_long_sentinel,
								       sizeof(expected));
								memcpy(expected, scratch_long_pattern,
								       qio_len);
								long_wr_ret = spihd_enhanced_xfer_lines(
									SPIHD_CMD_WRBUF, true,
									SPIHD_REG_QIO_SCRATCH,
									scratch_long_pattern, NULL,
									qio_len, 4);
								long_rd_ret = spihd_enhanced_xfer_lines(
									SPIHD_CMD_RDBUF, true,
									SPIHD_REG_QIO_SCRATCH, NULL,
									scratch_long_verify,
									sizeof(scratch_long_verify), 1);
								esp_info("SPI-HD QIO WRBUF length sweep: len=%u write=%d read=%d rx=%*ph %s\n",
									 qio_len, long_wr_ret, long_rd_ret,
									 (int)sizeof(scratch_long_verify),
									 scratch_long_verify,
									 (!long_wr_ret && !long_rd_ret &&
									  !memcmp(expected, scratch_long_verify,
										  sizeof(expected))) ?
									 "PASS" : "FAIL");
							}
						}

						/* Restore the sentinel for the canonical 20-byte case. */
						spihd_enhanced_xfer_lines(
							SPIHD_CMD_WRBUF, true,
							SPIHD_REG_QIO_SCRATCH,
							scratch_long_sentinel, NULL,
							sizeof(scratch_long_sentinel), 1);
						memset(scratch_long_verify, 0,
						       sizeof(scratch_long_verify));
						long_wr_ret = spihd_enhanced_xfer_lines(
							SPIHD_CMD_WRBUF, true,
							SPIHD_REG_QIO_SCRATCH,
							scratch_long_pattern, NULL,
							sizeof(scratch_long_pattern), 4);
						long_rd_ret = spihd_enhanced_xfer_lines(
							SPIHD_CMD_RDBUF, true,
							SPIHD_REG_QIO_SCRATCH, NULL,
							scratch_long_verify,
							sizeof(scratch_long_verify), 1);
						esp_info("SPI-HD QIO WRBUF 20B crosscheck: QIO-write=%d SIO-read=%d tx_hash=0x%08x rx_hash=0x%08x rx=%*ph %s\n",
							 long_wr_ret, long_rd_ret,
							 spihd_diag_hash(scratch_long_pattern,
									 sizeof(scratch_long_pattern)),
							 spihd_diag_hash(scratch_long_verify,
									 sizeof(scratch_long_verify)),
							 (int)sizeof(scratch_long_verify),
							 scratch_long_verify,
							 (!long_wr_ret && !long_rd_ret &&
							  !memcmp(scratch_long_pattern,
								  scratch_long_verify,
								  sizeof(scratch_long_pattern))) ?
							 "PASS" : "FAIL");
						/*
						 * Quad TX hypothesis: distinguish write-length
						 * truncation from an all-zero-heavy payload issue.
						 * Use a high-transition 20B pattern and then three
						 * individually addressed 9+9+2 quad transactions.
						 * The latter is diagnostic, never production batching.
						 */
						{
							static const u8 alt[20] = {
								0xa5, 0x5a, 0xc3, 0x3c, 0xf0,
								0x0f, 0x81, 0x18, 0x96, 0x69,
								0xe7, 0x7e, 0x24, 0x42, 0xbd,
								0xdb, 0x11, 0xee, 0x77, 0x88,
							};
							int split0, split1, split2, read_ret;

							spihd_enhanced_xfer_lines(SPIHD_CMD_WRBUF,
								true, SPIHD_REG_QIO_SCRATCH,
								scratch_long_sentinel, NULL,
								sizeof(scratch_long_sentinel), 1);
							long_wr_ret = spihd_enhanced_xfer_lines(
								SPIHD_CMD_WRBUF, true,
								SPIHD_REG_QIO_SCRATCH,
								alt, NULL, sizeof(alt), 4);
							long_rd_ret = spihd_enhanced_xfer_lines(
								SPIHD_CMD_RDBUF, true,
								SPIHD_REG_QIO_SCRATCH, NULL,
								scratch_long_verify,
								sizeof(scratch_long_verify), 1);
							esp_info("SPI-HD QIO WRBUF 20B entropy: write=%d read=%d rx=%*ph %s\n",
								 long_wr_ret, long_rd_ret,
								 (int)sizeof(scratch_long_verify),
								 scratch_long_verify,
								 (!long_wr_ret && !long_rd_ret &&
								  !memcmp(alt, scratch_long_verify,
									  sizeof(alt))) ? "PASS" : "FAIL");

							spihd_enhanced_xfer_lines(SPIHD_CMD_WRBUF,
								true, SPIHD_REG_QIO_SCRATCH,
								scratch_long_sentinel, NULL,
								sizeof(scratch_long_sentinel), 1);
							split0 = spihd_enhanced_xfer_lines(
								SPIHD_CMD_WRBUF, true,
								SPIHD_REG_QIO_SCRATCH,
								alt, NULL, 9, 4);
							split1 = spihd_enhanced_xfer_lines(
								SPIHD_CMD_WRBUF, true,
								SPIHD_REG_QIO_SCRATCH + 9,
								alt + 9, NULL, 9, 4);
							split2 = spihd_enhanced_xfer_lines(
								SPIHD_CMD_WRBUF, true,
								SPIHD_REG_QIO_SCRATCH + 18,
								alt + 18, NULL, 2, 4);
							read_ret = spihd_enhanced_xfer_lines(
								SPIHD_CMD_RDBUF, true,
								SPIHD_REG_QIO_SCRATCH, NULL,
								scratch_long_verify,
								sizeof(scratch_long_verify), 1);
							esp_info("SPI-HD QIO WRBUF 9+9+2 isolation: writes=%d,%d,%d read=%d rx=%*ph %s\n",
								 split0, split1, split2, read_ret,
								 (int)sizeof(scratch_long_verify),
								 scratch_long_verify,
								 (!split0 && !split1 && !split2 &&
								  !read_ret &&
								  !memcmp(alt, scratch_long_verify,
									  sizeof(alt))) ? "PASS" : "FAIL");
						}
					}
				}
			}
		}
	}

	return spihd_process_rx_batch(skb);
}

static bool spihd_data_ready(void)
{
	int value;

	if (!spi_hd.data_ready_gpio)
		return false;

	value = gpiod_get_value_cansleep(spi_hd.data_ready_gpio);
	if (value < 0) {
		esp_err("Failed to read SPI-HD data-ready GPIO: %d\n", value);
		return false;
	}
	return value != 0;
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
		((first->len | next->len) & 3) ||
		first->len + next->len > SPI_HD_TX_BATCH_MAX)
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
	if (!spi_hd_tx_batch || !spi_hd.tx_batch_buf ||
		spi_hd.tx_segment_max < SPI_HD_TX_BATCH_MAX ||
		spi_hd_tx_segment < SPI_HD_TX_BATCH_MAX)
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
	memcpy(spi_hd.tx_batch_buf, first->data, first->len);
	memcpy(spi_hd.tx_batch_buf + first->len, next->data, next->len);
	aggregate.data = spi_hd.tx_batch_buf;
	aggregate.len = first->len + next->len;
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
	bool is_cmd;
	bool progressed;
	unsigned int rx_batch, tx_batch;
	unsigned int rx_quota, tx_quota;
	int ret;

	if (!spi_hd.running || !spi_hd.spi || READ_ONCE(spi_hd.resyncing) ||
	    spihd_live_recovery_pending())
		return;

	/*
	 * RX and TX share one SPI controller/workqueue. Draining either
	 * source to empty can run forever under sustained opposite-direction
	 * traffic: cap each pass, then service the other side immediately.
	 * No new buffers, worker threads, retries, or scheduled delays.
	 */
	for (;;) {
		/* Refresh the pair each pass: sustained bidirectional load can
		 * keep this worker running indefinitely. A debugfs write must
		 * therefore apply without waiting for the work item to exit.
		 */
		esp_spi_hd_get_work_quotas(&rx_quota, &tx_quota);
		progressed = false;
		for (rx_batch = 0; rx_batch < rx_quota &&
		     spihd_data_ready(); rx_batch++) {
			if (!READ_ONCE(spi_hd.running) || READ_ONCE(spi_hd.resyncing) ||
			    spihd_live_recovery_pending())
				return;
			ret = spihd_receive_one();
			if (ret == -EAGAIN)
				break;
			if (ret) {
				esp_err("SPI-HD receive failed: %d\n", ret);
				esp_schedule_transport_recovery(spi_hd.adapter);
				return;
			}
			progressed = true;
		}

		if (!data_path || !spi_hd.adapter ||
		    atomic_read(&spi_hd.adapter->state) < ESP_CONTEXT_READY) {
			if (!progressed)
				return;
			continue;
		}

		for (tx_batch = 0; tx_batch < tx_quota; tx_batch++) {
			if (!READ_ONCE(spi_hd.running) || READ_ONCE(spi_hd.resyncing) ||
			    spihd_live_recovery_pending())
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
			is_cmd = spihd_get_cmd_info(skb, &cmd_code, &cmd_seq);
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
				esp_schedule_transport_recovery(spi_hd.adapter);
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
}

static irqreturn_t spihd_data_ready_irq(int irq, void *dev_id)
{
	if (spi_hd.running && spi_hd.workqueue)
		queue_work(spi_hd.workqueue, &spi_hd.work);
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
	u8 prio;

	if (!adapter || adapter != spi_hd.adapter || !skb || !skb->data) {
		if (skb)
			dev_kfree_skb_any(skb);
		return -EINVAL;
	}

	if (!data_path || !spi_hd.running ||
	    atomic_read(&adapter->state) < ESP_CONTEXT_READY) {
		/* Differentiate a firmware recovery TX quarantine from physical
		 * SPI rejection; this is rate-limited to avoid making the
		 * throughput diagnostic itself CPU/logging bound.
		 */
		if (raw_tp_mode)
			pr_warn_ratelimited("esp32_spi_hd raw TX not ready: state=%d data_path=%d running=%d flags=0x%lx\n",
				atomic_read(&adapter->state), data_path,
				spi_hd.running, adapter->state_flags);
		dev_kfree_skb_any(skb);
		return -ESHUTDOWN;
	}

	if (skb->len > SPI_HD_BUF_SIZE) {
		dev_kfree_skb_any(skb);
		return -EMSGSIZE;
	}

	if (atomic_read(&tx_pending) >= SPIHD_TX_MAX_PENDING) {
		dev_kfree_skb_any(skb);
		return -EBUSY;
	}

	header = (struct esp_payload_header *)skb->data;
	if (header->if_type == ESP_INTERNAL_IF)
		prio = PRIO_Q_HIGH;
	else if (header->if_type == ESP_HCI_IF)
		prio = PRIO_Q_MID;
	else
		prio = PRIO_Q_LOW;

	atomic_inc(&tx_pending);
	skb_queue_tail(&spi_hd.tx_q[prio], skb);
	queue_work(spi_hd.workqueue, &spi_hd.work);
	return 0;
}

static int spihd_hw_reset(void)
{
	int marker_ret;
	int ret;

	if (!spi_hd.reset_gpio)
		return -ENODEV;

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

	ret = gpiod_direction_output(spi_hd.reset_gpio, 0);
	if (ret)
		return ret;

	spi_hd.reset_gpio_driven = true;
	gpiod_set_value_cansleep(spi_hd.reset_gpio, 1);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(spi_hd.reset_gpio, 0);
	ret = gpiod_direction_input(spi_hd.reset_gpio);
	if (ret) {
		esp_err("Failed to release SPI-HD reset GPIO as input: %d\n", ret);
		return ret;
	}
	spi_hd.reset_gpio_driven = false;
	msleep(300);
	return 0;
}

static int spihd_reset_target(struct esp_adapter *adapter)
{
	if (!adapter || adapter != spi_hd.adapter)
		return -EINVAL;

	data_path = 0;
	if (spi_hd.workqueue)
		cancel_work_sync(&spi_hd.work);
	spihd_purge_queues();
	spi_hd.rx_byte_count = 0;
	spi_hd.tx_segment_count = 0;
	spi_hd.rx_credit_limit = 0;
	spi_hd.tx_inflight = false;
	spi_hd.rx_inflight = false;
	spi_hd.data_lines = 1;
	return spihd_hw_reset();
}

static int spihd_quiesce_for_fw_reset(struct esp_adapter *adapter)
{
	if (!adapter || adapter != spi_hd.adapter)
		return -EINVAL;

	data_path = 0;
	if (spi_hd.workqueue)
		cancel_work_sync(&spi_hd.work);
	spihd_purge_queues();
	return 0;
}

static int spihd_inject_resync(void)
{
	if (!spi_hd.adapter || !spi_hd.running)
		return -ENODEV;

	esp_info("SPI-HD injected transport resync\n");
	esp_schedule_transport_recovery(spi_hd.adapter);
	return 0;
}

static int spihd_inject_resync_set(const char *val, const struct kernel_param *kp)
{
	bool inject;
	int ret;

	ret = kstrtobool(val, &inject);
	if (ret)
		return ret;
	return inject ? spihd_inject_resync() : 0;
}

static void spihd_test_resync_fn(struct work_struct *work)
{
	if (!spi_hd.running || spi_hd_test_resync_next >= spi_hd_test_resync_count)
		return;
	spihd_inject_resync();
	spi_hd_test_resync_next++;
	if (spi_hd_test_resync_next < spi_hd_test_resync_count)
		schedule_delayed_work(&spihd_test_resync_work,
			msecs_to_jiffies(spi_hd_test_resync_ms[spi_hd_test_resync_next]));
}

static void spihd_ab_log(const char *what, unsigned int phase)
{
	esp_info("SPI-HD A/B %s phase=%u poll_us=%u fastpath=%u segments=%u credit_reads=%u credit_sleeps=%u resyncs=%u\n",
		 what, phase, READ_ONCE(spi_hd_credit_poll_us),
		 READ_ONCE(spi_hd_sio_fastpath),
		 spi_hd_stat_segments, spi_hd_stat_credit_reads,
		 spi_hd_stat_credit_sleeps, spi_hd_stat_resyncs);
}

static void spihd_ab_work_fn(struct work_struct *work)
{
	struct spihd_ab_phase *p;

	if (!spi_hd.running)
		return;
	if (spihd_ab_next >= spihd_ab_count) {
		spihd_ab_log("done", spihd_ab_next);
		return;
	}

	p = &spihd_ab[spihd_ab_next];
	if (p->poll_us < 0) {
		spihd_inject_resync();
	} else {
		WRITE_ONCE(spi_hd_credit_poll_us, p->poll_us);
		WRITE_ONCE(spi_hd_sio_fastpath, p->fastpath);
	}
	spihd_ab_log(p->poll_us < 0 ? "inject" : "start", spihd_ab_next);
	spihd_ab_next++;
	schedule_delayed_work(&spihd_ab_work, msecs_to_jiffies(p->secs * 1000));
}

static void spihd_ab_start(void)
{
	char *buf, *cur, *tok, *secs, *fast;
	unsigned int n = 0;
	unsigned int value;

	if (!spi_hd_ab_phases || !*spi_hd_ab_phases || spihd_ab_count)
		return;

	buf = kstrdup(spi_hd_ab_phases, GFP_KERNEL);
	if (!buf)
		return;

	cur = buf;
	while ((tok = strsep(&cur, ",")) != NULL) {
		secs = strchr(tok, ':');
		if (!secs || n >= SPIHD_AB_MAX_PHASES)
			goto invalid;
		*secs++ = '\0';
		if (kstrtouint(secs, 10, &spihd_ab[n].secs) ||
		    !spihd_ab[n].secs || spihd_ab[n].secs > 120)
			goto invalid;
		if (!strcmp(tok, "inject")) {
			spihd_ab[n].poll_us = -1;
		} else {
			fast = strchr(tok, '/');
			spihd_ab[n].fastpath = READ_ONCE(spi_hd_sio_fastpath);
			if (fast) {
				*fast++ = '\0';
				if (kstrtouint(fast, 10, &value) || value > 3)
					goto invalid;
				spihd_ab[n].fastpath = value;
			}
			if (kstrtouint(tok, 10, &value) || value > 100000)
				goto invalid;
			spihd_ab[n].poll_us = value;
		}
		n++;
	}
	kfree(buf);

	spihd_ab_count = n;
	spihd_ab_next = 0;
	schedule_delayed_work(&spihd_ab_work, 0);
	return;

invalid:
	kfree(buf);
	esp_err("SPI-HD invalid spi_hd_ab_phases \"%s\"\n", spi_hd_ab_phases);
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
	u32 ready = 0, rx_credits = 0, tx_bytes = 0;
	s32 credit_delta;
	u32 pending;
	int ret;

	ret = spihd_read_reg_stable_lines(SPIHD_REG_SLAVE_READY, &ready, 1);
	if (ret)
		return ret;
	if ((ready & 0xff) != SPIHD_STATE_SLAVE_READY)
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

	tx_bytes &= SPIHD_TX_BUF_LEN_MASK;
	credit_delta = (s32)(rx_credits - spi_hd.tx_segment_count);
	pending = (tx_bytes - spi_hd.rx_byte_count) & SPIHD_TX_BUF_LEN_MASK;
	if (credit_delta < 0 || credit_delta > SPIHD_LIVE_CREDIT_MAX ||
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

	spi_hd.rx_credit_limit = 0;
	data_path = 1;
	atomic_set(&adapter->state, ESP_CONTEXT_READY);
	spi_hd_stat_resyncs++;
	esp_info("SPI-HD live resync: lines=%u rx_credits=%u sent=%u e2h_pending=%u\n",
		 spi_hd.data_lines, rx_credits, spi_hd.tx_segment_count,
		 pending);
	return 1;
}

static int spihd_recover_transport(struct esp_adapter *adapter)
{
	int ret;

	if (!adapter || adapter != spi_hd.adapter || !spi_hd.spi)
		return -ENODEV;
	esp_dbg("SPI-HD recovery: lines=%u running=%u datapath=%u tx-inflight=%u rx-inflight=%u sent=%u consumed=0x%06x credits=%u queued=%d\n",
		spi_hd.data_lines, spi_hd.running, data_path,
		spi_hd.tx_inflight, spi_hd.rx_inflight,
		spi_hd.tx_segment_count, spi_hd.rx_byte_count,
		spi_hd.rx_credit_limit, atomic_read(&tx_pending));

	if (test_bit(ESP_INIT_DONE, &adapter->state_flags) &&
	    !test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags)) {
		WRITE_ONCE(spi_hd.resyncing, true);
		if (spi_hd.workqueue)
			cancel_work_sync(&spi_hd.work);
		ret = spihd_resync_live_session(adapter);
		WRITE_ONCE(spi_hd.resyncing, false);
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
	}

	/*
	 * A spontaneous firmware reset returns the SPI-HD command decoder to
	 * single-line bring-up even though the host may have been operating in
	 * QIO. Always rebase both cumulative counters and line width before
	 * polling SLAVE_READY; the new boot event will switch us back to QIO.
	 */
	data_path = 0;
	spi_hd.data_lines = 1;
	spi_hd.rx_byte_count = 0;
	spi_hd.tx_segment_count = 0;
	spi_hd.rx_credit_limit = 0;
	spi_hd.tx_inflight = false;
	spi_hd.rx_inflight = false;

	ret = spihd_wait_slave_ready();
	if (ret)
		return ret;
	ret = spihd_open_slave_datapath();
	if (ret)
		return ret;

	data_path = 1;
	if (spi_hd.workqueue)
		queue_work(spi_hd.workqueue, &spi_hd.work);
	return 0;
}

static int spihd_reinit_after_fw_reset(struct esp_adapter *adapter)
{
	if (!adapter || adapter != spi_hd.adapter)
		return -EINVAL;

	data_path = 1;
	atomic_set(&adapter->state, ESP_CONTEXT_READY);
	if (spi_hd.workqueue)
		queue_work(spi_hd.workqueue, &spi_hd.work);
	return 0;
}

static void spihd_flush_bt_traffic(struct esp_adapter *adapter)
{
	struct sk_buff *skb, *tmp;
	struct sk_buff_head free_q;
	unsigned long flags;
	bool has_work;
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

	has_work = spihd_data_ready() ||
		   !skb_queue_empty(&spi_hd.tx_q[PRIO_Q_HIGH]) ||
		   !skb_queue_empty(&spi_hd.tx_q[PRIO_Q_MID]) ||
		   !skb_queue_empty(&spi_hd.tx_q[PRIO_Q_LOW]);
	if (spi_hd.running && data_path && has_work && spi_hd.workqueue)
		queue_work(spi_hd.workqueue, &spi_hd.work);
}

static void spihd_resume_after_live_recovery(struct esp_adapter *adapter)
{
	if (adapter != spi_hd.adapter)
		return;
	/* Queue unconditionally: either a pending TX skb or an already-high
	 * Data Ready level may need service without another rising IRQ edge.
	 */
	if (READ_ONCE(spi_hd.running) && spi_hd.workqueue)
		queue_work(spi_hd.workqueue, &spi_hd.work);
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
	u32 max_tx_len;
	u32 max_rx_len;
	int ret;

	if (!spi || !spi_hd.adapter || spi_hd.spi)
		return -ENODEV;
	{
		unsigned int rx_quota, tx_quota;

		esp_spi_hd_get_work_quotas(&rx_quota, &tx_quota);
		esp_info("SPI-HD worker service quotas: E2H=%u H2E=%u (debugfs tunable)\n",
			 rx_quota, tx_quota);
	}

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
	if (spi->mode & ~(SPI_CPHA | SPI_CPOL | SPI_CS_HIGH |
			  SPI_TX_QUAD | SPI_RX_QUAD)) {
		esp_err("Unsupported SPI-HD mode flags 0x%x\n", spi->mode);
		return -EINVAL;
	}

	if (spi_hd_force_mode0) {
		spi->mode &= ~(SPI_CPOL | SPI_CPHA);
		esp_info("SPI-HD diagnostic: forcing host SPI mode 0\n");
	}

	spi->max_speed_hz = spi_hd.spi_max_hz;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	esp_info("SPI-HD active mode: %u\n",
		 (unsigned int)(spi->mode & (SPI_CPOL | SPI_CPHA)));
	spi_hd.spi = spi;
	spi_set_drvdata(spi, &spi_hd);

	esp_info("SPI-HD controller=%s device=%s mode=0x%x flags=0x%x bpw-mask=0x%x board-max=%uHz bus-max=%uHz max-transfer=%zu max-message=%zu quad=%u full-duplex=%u\n",
		 dev_name(&SPIHD_CONTROLLER(spi)->dev), dev_name(&spi->dev),
		 spi->mode, SPIHD_CONTROLLER(spi)->flags,
		 SPIHD_CONTROLLER(spi)->bits_per_word_mask, board_max_hz,
		 spi_hd.spi_max_hz, spi_max_transfer_size(spi),
		 spi_max_message_size(spi), spihd_quad_available(spi),
		 spihd_full_duplex());
	if ((spi_hd_qio_tx_word32 || spi_hd_qio_rx_word32) &&
	    !spihd_bpw_supported(spi, 32))
		esp_warn("SPI-HD 32-bit data words unavailable; using 8-bit words\n");

	spi_hd.reset_gpio = devm_gpiod_get(&spi->dev, "reset", GPIOD_IN);
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
	esp_dbg("SPI-HD GPIO: reset-active-low=%u ready-active-low=%u ready-can-sleep=%u irq=%d edge=%s\n",
		gpiod_is_active_low(spi_hd.reset_gpio),
		gpiod_is_active_low(spi_hd.data_ready_gpio),
		gpiod_cansleep(spi_hd.data_ready_gpio), spi_hd.data_ready_irq,
		gpiod_is_active_low(spi_hd.data_ready_gpio) ? "falling" : "rising");

	ret = devm_request_irq(&spi->dev, spi_hd.data_ready_irq,
			       spihd_data_ready_irq,
			       spihd_gpio_irq_flags(spi_hd.data_ready_gpio),
			       "ESP_SPI_HD_DATA_READY", spi);
	if (ret)
		goto fail;

	clear_bit(ESP_TRANSPORT_REMOVING, &spi_hd.adapter->state_flags);
	clear_bit(ESP_CLEANUP_IN_PROGRESS, &spi_hd.adapter->state_flags);

	if (!spi_hd_skip_probe_reset) {
		ret = spihd_hw_reset();
		if (ret)
			goto fail;
	} else {
		esp_info("SPI-HD probe: external reset already performed; skipping reset GPIO\n");
	}

	ret = spihd_wait_slave_ready();
	if (ret)
		goto fail;

	ret = spihd_read_reg_stable(SPIHD_REG_MAX_TX_BUF_LEN, &max_tx_len);
	if (ret || !max_tx_len || max_tx_len > 6400 ||
	    max_tx_len > READ_ONCE(spi_hd_rx_batch_bytes)) {
		esp_err("Invalid SPI-HD slave TX limit: ret=%d len=%u\n",
			ret, ret ? 0 : max_tx_len);
		ret = ret ? ret : -EPROTO;
		goto fail;
	}

	ret = spihd_read_reg_stable(SPIHD_REG_MAX_RX_BUF_LEN, &max_rx_len);
	if (ret || max_rx_len < SPI_HD_TX_SEGMENT_MIN ||
	    max_rx_len > SPI_HD_TX_BATCH_MAX) {
		esp_err("Invalid SPI-HD slave RX segment limit: ret=%d len=%u\n",
			ret, ret ? 0 : max_rx_len);
		ret = ret ? ret : -EPROTO;
		goto fail;
	}

	/* RDDMA consumes one published descriptor, so cannot be shortened on
	 * the host. Fail before opening the datapath if it cannot fit intact.
	 */
	if (max_tx_len > spi_max_transfer_size(spi) ||
	    spi_max_message_size(spi) < 6 ||
	    max_tx_len > spi_max_message_size(spi) - 6) {
		esp_err("SPI-HD slave TX descriptor %u exceeds controller capacity; reduce firmware TX batch size\n",
			max_tx_len);
		ret = -EMSGSIZE;
		goto fail;
	}
	max_rx_len = min_t(size_t, max_rx_len, spi_max_transfer_size(spi));
	max_rx_len = min_t(size_t, max_rx_len, spi_max_message_size(spi) - 6);
	if (max_rx_len < SPI_HD_TX_SEGMENT_MIN) {
		ret = -EMSGSIZE;
		goto fail;
	}
	spihd_rx_limit = max_tx_len;
	spihd_rx_batch_logs = 0;
	spi_hd.tx_segment_max = max_rx_len;
	esp_dbg("SPI-HD negotiated: slave-tx=%u host-tx-segment-max=%u append=%u no-ack=%u mirror=%u tx-batch=%u rx-batch=%u fastpath=0x%x\n",
		max_tx_len, max_rx_len, spi_hd_append_tx, spi_hd_append_no_ack,
		spi_hd_tx_len_mirror, spi_hd_tx_batch, spi_hd_rx_batch_bytes,
		spi_hd_sio_fastpath);
	if (spi_hd_tx_segment > spi_hd.tx_segment_max) {
		esp_warn("SPI-HD TX segment %u exceeds slave limit %u; clamping\n",
			 spi_hd_tx_segment, spi_hd.tx_segment_max);
		spi_hd_tx_segment = spi_hd.tx_segment_max;
	}

	/*
	 * Make the IRQ/work path live before opening the slave datapath. Opening
	 * the datapath allows firmware to queue its boot event immediately; if
	 * running were set afterwards, that first Data Ready edge could be lost.
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
	spi_hd.running = true;

	ret = spihd_open_slave_datapath();
	if (ret)
		goto fail;

	data_path = 1;

	esp_info("ESP SPI-HD/QIO bound: clock=%uHz requested=%uMHz tx-segment=%u slave-tx-max=%u\n",
		 spihd_effective_speed_hz(), spi_hd.spi_clk_mhz,
		 spi_hd_tx_segment, max_tx_len);

	if (spihd_data_ready())
		queue_work(spi_hd.workqueue, &spi_hd.work);
	/*
	 * C5 firmware with Wi-Fi/BT enabled reaches its first boot event after
	 * more than the generic 3 s transport watchdog on this bring-up path.
	 * Avoid resetting a healthy slave while PHY/BT initialization is still
	 * in progress; later recovery continues to use the common watchdog.
	 */
	schedule_delayed_work(&spi_hd.adapter->fw_recovery_work,
			      msecs_to_jiffies(SPIHD_INITIAL_BOOT_TIMEOUT_MS));
	return 0;

fail:
	spi_hd.running = false;
	data_path = 0;
	if (spi_hd.workqueue)
		cancel_work_sync(&spi_hd.work);
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

	spi_hd.running = false;
	data_path = 0;
	cancel_delayed_work_sync(&spihd_test_resync_work);
	cancel_delayed_work_sync(&spihd_ab_work);
	if (adapter) {
		set_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags);
		set_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
		clear_bit(ESP_INIT_DONE, &adapter->state_flags);
		atomic_set(&adapter->state, ESP_CONTEXT_DISABLED);
		cancel_delayed_work_sync(&adapter->fw_recovery_work);
	}

	if (spi_hd.workqueue)
		cancel_work_sync(&spi_hd.work);
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
	if (!adapter || adapter != spi_hd.adapter || !spi_clk_mhz)
		return -EINVAL;

	spi_hd.spi_clk_mhz =
		min_t(u32, spi_clk_mhz, SPI_HD_MAX_CLK_MHZ);
	if (spi_hd.spi)
		spi_hd.spi->max_speed_hz = spi_hd.spi_max_hz;
	/*
	 * The 10 MHz bring-up clock may change after the C5 advertises its
	 * supported speed in the boot TLV. Log the effective, DT-capped rate so
	 * high-clock qualification cannot accidentally validate the boot rate.
	 */
	esp_info("SPI-HD post-boot clock: advertised=%uMHz effective=%uHz dt_limit=%uHz\n",
		 spi_clk_mhz, spihd_effective_speed_hz(), spi_hd.spi_max_hz);
	spihd_ab_start();
	spi_hd_test_resync_next = 0;
	if (raw_tp_mode && spi_hd_test_resync_count)
		schedule_delayed_work(&spihd_test_resync_work,
			msecs_to_jiffies(spi_hd_test_resync_ms[0]));
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

	/* Validate startup module parameters before creating the worker. */
	ret = esp_spi_hd_set_work_quotas(spi_hd_rx_work_quota,
					spi_hd_tx_work_quota);
	if (ret) {
		esp_err("Invalid SPI-HD work quotas E2H=%u H2E=%u (valid 1..8)\n",
			spi_hd_rx_work_quota, spi_hd_tx_work_quota);
		return ret;
	}

	memset(&spi_hd, 0, sizeof(spi_hd));
	spi_hd.adapter = adapter;
	spi_hd.requested_clk_mhz = speed ?
		min_t(u32, speed, SPI_HD_MAX_CLK_MHZ) :
		SPI_HD_INITIAL_CLK_MHZ;
	spi_hd.spi_clk_mhz = spi_hd.requested_clk_mhz;
	spi_hd.data_lines = 1;
	spi_hd.sio_buf = kmalloc(spihd_control_offset() +
		2 * ALIGN(SPIHD_CONTROL_BYTES, spihd_control_alignment()), GFP_KERNEL);
	if (spi_hd_tx_batch)
		spi_hd.tx_batch_buf = kmalloc(SPI_HD_TX_BATCH_MAX, GFP_KERNEL);
	if (!spi_hd.sio_buf || (spi_hd_tx_batch && !spi_hd.tx_batch_buf)) {
		kfree(spi_hd.sio_buf);
		kfree(spi_hd.tx_batch_buf);
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
		kfree(spi_hd.sio_buf);
		kfree(spi_hd.tx_batch_buf);
		spi_hd.tx_batch_buf = NULL;
		spi_hd.sio_buf = NULL;
		return -ENOMEM;
	}
	INIT_WORK(&spi_hd.work, spihd_work);
	INIT_DELAYED_WORK(&spihd_ab_work, spihd_ab_work_fn);
	spihd_ab_count = 0;

	ret = spi_register_driver(&spihd_driver);
	if (ret) {
		destroy_workqueue(spi_hd.workqueue);
		spi_hd.workqueue = NULL;
		kfree(spi_hd.sio_buf);
		kfree(spi_hd.tx_batch_buf);
		spi_hd.tx_batch_buf = NULL;
		spi_hd.sio_buf = NULL;
		return ret;
	}
	spi_hd_driver_registered = true;
	return 0;
}

void esp_deinit_interface_layer(void)
{
	data_path = 0;
	if (spi_hd_driver_registered) {
		spi_unregister_driver(&spihd_driver);
		spi_hd_driver_registered = false;
	}
	if (spi_hd.workqueue) {
		cancel_delayed_work_sync(&spihd_ab_work);
		cancel_work_sync(&spi_hd.work);
		spihd_purge_queues();
		destroy_workqueue(spi_hd.workqueue);
		spi_hd.workqueue = NULL;
	}
	kfree(spi_hd.sio_buf);
	kfree(spi_hd.tx_batch_buf);
	memset(&spi_hd, 0, sizeof(spi_hd));
}
