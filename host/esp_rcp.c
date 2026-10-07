// SPDX-License-Identifier: GPL-2.0-only
/*
 * Opaque IEEE 802.15.4 RCP byte-stream endpoint for ESP-Hosted.
 * Thread/Zigbee/Spinel policy deliberately stays outside the kernel driver.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>

#include "utils.h"
#include "esp_api.h"
#include "esp_if.h"
#include "esp_kernel_port.h"
#include "esp_rcp_api.h"

#define ESP_RCP_DEVICE_NAME        "esp_rcp0"
#define ESP_RCP_RX_QUEUE_LIMIT     64
#define ESP_RCP_TX_INFLIGHT_LIMIT  64
#define ESP_RCP_HOSTED_CHUNK_MAX   1400
#define ESP_RCP_SESSION_ACK_TIMEOUT_MS 2000

static DEFINE_MUTEX(rcp_lifecycle_lock);
static DEFINE_MUTEX(rcp_lock);
static DEFINE_MUTEX(rcp_read_lock);
static DEFINE_MUTEX(rcp_write_lock);
static DECLARE_WAIT_QUEUE_HEAD(rcp_read_wait);
static DECLARE_WAIT_QUEUE_HEAD(rcp_write_wait);
static DECLARE_WAIT_QUEUE_HEAD(rcp_session_wait);
static struct sk_buff_head rcp_rx_q;
static struct sk_buff_head rcp_early_q;
static bool rcp_queue_initialized;
static bool rcp_boot_pending;
static bool rcp_early_overflow;
static struct esp_adapter *rcp_boot_adapter;
static bool rcp_registered;
static bool rcp_opened;
static bool rcp_ever_opened;
static bool rcp_opening_first;
static u32 rcp_generation;
static struct esp_adapter *rcp_adapter;
static u64 rcp_session_acked_nonce;
static atomic_t rcp_session_active = ATOMIC_INIT(0);
static atomic_t rcp_tx_inflight = ATOMIC_INIT(0);

struct esp_rcp_file_ctx {
	u32 generation;
};

static void esp_rcp_init_queues_locked(void)
{
	if (rcp_queue_initialized)
		return;
	skb_queue_head_init(&rcp_rx_q);
	skb_queue_head_init(&rcp_early_q);
	rcp_queue_initialized = true;
}

static bool esp_rcp_session_ready(struct esp_adapter *adapter)
{
	return adapter &&
		(adapter->ext_capabilities & ESP_EXT_CAP_RCP_ANY) &&
		test_bit(ESP_INIT_DONE, &adapter->state_flags) &&
		!test_bit(ESP_ALLOW_RECONSTRUCT, &adapter->state_flags) &&
		!test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) &&
		!test_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags) &&
		!test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags) &&
		!test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags) &&
		!test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) &&
		atomic_read(&adapter->state) >= ESP_CONTEXT_READY;
}

static bool esp_rcp_file_is_current(struct file *file)
{
	struct esp_rcp_file_ctx *ctx = file ? file->private_data : NULL;
	bool session_current;

	mutex_lock(&rcp_lock);
	session_current = ctx && rcp_registered && rcp_opened &&
			  ctx->generation == rcp_generation &&
			  atomic_read(&rcp_session_active);
	mutex_unlock(&rcp_lock);
	return session_current;
}

static bool esp_rcp_file_is_current_lockless(struct file *file)
{
	struct esp_rcp_file_ctx *ctx = file ? file->private_data : NULL;

	return ctx && READ_ONCE(rcp_registered) && READ_ONCE(rcp_opened) &&
		ctx->generation == READ_ONCE(rcp_generation) &&
		atomic_read(&rcp_session_active);
}

static struct esp_adapter *esp_rcp_adapter_snapshot(struct file *file)
{
	struct esp_rcp_file_ctx *ctx = file ? file->private_data : NULL;
	struct esp_adapter *adapter = NULL;

	mutex_lock(&rcp_lock);
	if (ctx && rcp_registered && rcp_opened &&
	    ctx->generation == rcp_generation &&
	    atomic_read(&rcp_session_active))
		adapter = rcp_adapter;
	mutex_unlock(&rcp_lock);
	return adapter;
}

static struct sk_buff *esp_rcp_dequeue_current(struct file *file,
					      bool *session_current)
{
	struct esp_rcp_file_ctx *ctx = file ? file->private_data : NULL;
	struct sk_buff *skb = NULL;

	mutex_lock(&rcp_lock);
	*session_current = ctx && rcp_registered && rcp_opened &&
			   ctx->generation == rcp_generation &&
			   atomic_read(&rcp_session_active);
	if (*session_current)
		skb = skb_dequeue(&rcp_rx_q);
	mutex_unlock(&rcp_lock);
	return skb;
}

static bool esp_rcp_requeue_current(struct file *file, struct sk_buff *skb)
{
	struct esp_rcp_file_ctx *ctx = file ? file->private_data : NULL;
	bool requeued = false;

	mutex_lock(&rcp_lock);
	if (ctx && skb && rcp_registered && rcp_opened &&
	    ctx->generation == rcp_generation &&
	    atomic_read(&rcp_session_active)) {
		skb_queue_head(&rcp_rx_q, skb);
		requeued = true;
	}
	mutex_unlock(&rcp_lock);
	return requeued;
}

static void esp_rcp_release_tx_slot(void)
{
	atomic_dec(&rcp_tx_inflight);
	wake_up_interruptible(&rcp_write_wait);
}

static void esp_rcp_tx_destructor(struct sk_buff *skb)
{
	(void)skb;
	esp_rcp_release_tx_slot();
}

static int esp_rcp_reserve_tx_slot(struct file *file)
{
	int ret;

	for (;;) {
		if (!esp_rcp_file_is_current_lockless(file))
			return -ENODEV;

		if (atomic_read(&rcp_tx_inflight) < ESP_RCP_TX_INFLIGHT_LIMIT) {
			if (atomic_inc_return(&rcp_tx_inflight) <= ESP_RCP_TX_INFLIGHT_LIMIT)
				return 0;
			esp_rcp_release_tx_slot();
		}

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(rcp_write_wait,
				atomic_read(&rcp_tx_inflight) < ESP_RCP_TX_INFLIGHT_LIMIT ||
				!esp_rcp_file_is_current_lockless(file));
		if (ret)
			return ret;
	}
}

static int esp_rcp_send_session_start(struct esp_adapter *adapter);

static int esp_rcp_open(struct inode *inode, struct file *file)
{
	struct esp_rcp_file_ctx *ctx;
	struct esp_adapter *adapter = NULL;
	long ack_wait = 0;
	bool recover = false;
	int ret = 0;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	/*
	 * Hold lifecycle serialization until the nonce marker is confirmed. RX
	 * confirmation only takes rcp_lock, so it can complete while open waits;
	 * reset/deinit waits here for at most the bounded ACK timeout.
	 */
	mutex_lock(&rcp_lifecycle_lock);
	mutex_lock(&rcp_lock);
	if (!rcp_registered || !atomic_read(&rcp_session_active) ||
	    !esp_rcp_session_ready(rcp_adapter))
		ret = -ENODEV;
	else if (rcp_opened)
		ret = -EBUSY;
	else {
		adapter = rcp_adapter;
		ctx->generation = rcp_generation;
		file->private_data = ctx;

		/*
		 * Every userspace open is a fresh stream boundary even though the
		 * firmware boot nonce is intentionally stable for the incarnation.
		 * DATA from a previous close must never survive into this opener.
		 *
		 * Only the first open is allowed to quarantine pre-ACK DATA: that is
		 * the one-time OpenThread RESET/status which firmware deliberately
		 * releases immediately before its first nonce ACK.
		 */
		skb_queue_purge(&rcp_rx_q);
		rcp_opening_first = !rcp_ever_opened;
		if (!rcp_opening_first)
			skb_queue_purge(&rcp_early_q);
		rcp_opened = true;
		rcp_session_acked_nonce = 0;

		/*
		 * Publish the current firmware nonce on every open. The matching ACK
		 * is the ordering boundary for all reopen DATA.
		 */
		ret = esp_rcp_send_session_start(adapter);
		if (ret) {
			rcp_opened = false;
			rcp_opening_first = false;
			file->private_data = NULL;
		}
	}
	mutex_unlock(&rcp_lock);

	if (!ret && adapter) {
		ack_wait = wait_event_timeout(
			rcp_session_wait,
			READ_ONCE(rcp_session_acked_nonce) ==
				READ_ONCE(adapter->rcp_session_nonce),
			msecs_to_jiffies(ESP_RCP_SESSION_ACK_TIMEOUT_MS));

		mutex_lock(&rcp_lock);
		if (!ack_wait ||
		    !rcp_registered || !rcp_opened ||
		    ctx->generation != rcp_generation ||
		    rcp_adapter != adapter ||
		    rcp_session_acked_nonce != adapter->rcp_session_nonce) {
			if (rcp_registered && rcp_opened &&
			    ctx->generation == rcp_generation &&
			    rcp_adapter == adapter) {
				rcp_opened = false;
				rcp_opening_first = false;
				skb_queue_purge(&rcp_rx_q);
				skb_queue_purge(&rcp_early_q);
				file->private_data = NULL;
			}
			ret = -ETIMEDOUT;
			recover = true;
		}
		mutex_unlock(&rcp_lock);

		if (recover) {
			esp_err("RCP session confirmation timeout nonce=%016llx\n",
				(unsigned long long)adapter->rcp_session_nonce);
			if (adapter->if_ops && adapter->if_ops->flush_rcp_traffic)
				adapter->if_ops->flush_rcp_traffic(adapter);
			esp_schedule_fw_reset_recovery(adapter);
			esp_request_firmware_restart(adapter);
		}
	}

	mutex_unlock(&rcp_lifecycle_lock);

	if (ret)
		kfree(ctx);
	return ret;
}

static int esp_rcp_release(struct inode *inode, struct file *file)
{
	struct esp_rcp_file_ctx *ctx = file->private_data;
	struct esp_adapter *adapter = NULL;
	bool session_current = false;

	mutex_lock(&rcp_lifecycle_lock);
	mutex_lock(&rcp_lock);
	if (ctx && rcp_registered && rcp_opened &&
	    ctx->generation == rcp_generation) {
		session_current = true;
		rcp_opened = false;
		rcp_opening_first = false;
		rcp_session_acked_nonce = 0;
		adapter = rcp_adapter;
		skb_queue_purge(&rcp_rx_q);
		skb_queue_purge(&rcp_early_q);
	}
	file->private_data = NULL;
	mutex_unlock(&rcp_lock);

	if (session_current && adapter && adapter->if_ops &&
	    adapter->if_ops->flush_rcp_traffic)
		adapter->if_ops->flush_rcp_traffic(adapter);

	/*
	 * ESP-IDF v6.1 has no safe hot-reset/fence for a running native-radio NCP.
	 * A transport flush can order Host->ESP ownership, but it cannot prove that
	 * OpenThread has finished every asynchronous effect of bytes already
	 * consumed from the custom transport. Reincarnate the complete firmware
	 * after a live userspace RCP session closes so the next open necessarily
	 * binds to a fresh boot nonce and a fresh OpenThread backend.
	 *
	 * This also defines the SPI/SDIO/USB close linearization point: queued and
	 * worker-owned stateful traffic is flushed/waited first, then the old
	 * firmware incarnation is destroyed. A subsequent open is rejected while
	 * recovery is pending by esp_rcp_session_ready().
	 */
	if (session_current && adapter &&
	    !test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) &&
	    !test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags)) {
		esp_info("RCP userspace session closed; reincarnating firmware to fence backend state\n");
		esp_schedule_fw_reset_recovery(adapter);
		esp_request_firmware_restart(adapter);
	}
	mutex_unlock(&rcp_lifecycle_lock);

	if (session_current) {
		wake_up_interruptible(&rcp_read_wait);
		wake_up_interruptible(&rcp_write_wait);
	}
	kfree(ctx);
	return 0;
}

static ssize_t esp_rcp_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct sk_buff *skb;
	ssize_t result;
	size_t n;
	int ret;

	if (!count)
		return 0;

	ret = mutex_lock_interruptible(&rcp_read_lock);
	if (ret)
		return ret;

	for (;;) {
		bool session_current;

		skb = esp_rcp_dequeue_current(file, &session_current);
		if (!session_current) {
			result = -ENODEV;
			goto out_unlock;
		}
		if (skb)
			break;
		if (file->f_flags & O_NONBLOCK) {
			result = -EAGAIN;
			goto out_unlock;
		}
		ret = wait_event_interruptible(rcp_read_wait,
				!skb_queue_empty(&rcp_rx_q) ||
				!esp_rcp_file_is_current_lockless(file));
		if (ret) {
			result = ret;
			goto out_unlock;
		}
	}

	n = min_t(size_t, count, skb->len);
	if (copy_to_user(buf, skb->data, n)) {
		if (!esp_rcp_requeue_current(file, skb))
			dev_kfree_skb_any(skb);
		result = -EFAULT;
		goto out_unlock;
	}

	skb_pull(skb, n);
	if (skb->len) {
		if (!esp_rcp_requeue_current(file, skb))
			dev_kfree_skb_any(skb);
	} else {
		dev_kfree_skb_any(skb);
	}
	result = n;

out_unlock:
	mutex_unlock(&rcp_read_lock);
	return result;
}

static int esp_rcp_send_chunk(struct file *file, struct esp_adapter *adapter,
			      const char __user *buf, size_t len)
{
	struct esp_rcp_file_ctx *ctx = file ? file->private_data : NULL;
	struct esp_payload_header *hdr;
	struct sk_buff *skb;
	size_t total_len;
	u8 pad_len;
	int ret;

	ret = esp_rcp_reserve_tx_slot(file);
	if (ret)
		return ret;

	pad_len = sizeof(*hdr);
	total_len = len + sizeof(*hdr);
	pad_len += (SKB_DATA_ADDR_ALIGNMENT - (total_len % SKB_DATA_ADDR_ALIGNMENT)) %
		   SKB_DATA_ADDR_ALIGNMENT;

	skb = esp_if_alloc_skb(adapter, len + pad_len);
	if (!skb) {
		esp_rcp_release_tx_slot();
		return -ENOMEM;
	}
	skb->destructor = esp_rcp_tx_destructor;

	skb_put(skb, len + pad_len);
	memset(skb->data, 0, pad_len);
	if (copy_from_user(skb->data + pad_len, buf, len)) {
		dev_kfree_skb_any(skb);
		return -EFAULT;
	}

	hdr = (struct esp_payload_header *)skb->data;
	hdr->if_type = ESP_RCP_IF;
	hdr->if_num = 0;
	hdr->len = esp_wire_cpu_to_le16(len);
	hdr->offset = esp_wire_cpu_to_le16(pad_len);
	hdr->packet_type = PACKET_TYPE_DATA;
	if (adapter->capabilities & ESP_CHECKSUM_ENABLED)
		hdr->checksum = esp_wire_cpu_to_le16(
				compute_checksum(skb->data, len + pad_len));

	/*
	 * Serialize final publication with generation changes. User copying and
	 * allocation happen above without rcp_lock, so reset teardown is never
	 * held up by a userspace page fault.
	 */
	mutex_lock(&rcp_lock);
	if (!ctx || !rcp_registered || !rcp_opened ||
	    ctx->generation != rcp_generation ||
	    !atomic_read(&rcp_session_active) ||
	    rcp_adapter != adapter || !esp_rcp_session_ready(adapter)) {
		mutex_unlock(&rcp_lock);
		dev_kfree_skb_any(skb);
		return -ENODEV;
	}
	ret = esp_send_packet(adapter, skb);
	mutex_unlock(&rcp_lock);
	return ret;
}

static ssize_t esp_rcp_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct esp_adapter *adapter;
	ssize_t result;
	size_t done = 0;
	int ret;

	if (!count)
		return 0;

	ret = mutex_lock_interruptible(&rcp_write_lock);
	if (ret)
		return ret;

	adapter = esp_rcp_adapter_snapshot(file);
	if (!esp_rcp_session_ready(adapter)) {
		result = -ENODEV;
		goto out_unlock;
	}

	while (done < count) {
		size_t chunk = min_t(size_t, count - done, ESP_RCP_HOSTED_CHUNK_MAX);

		if (!esp_rcp_file_is_current(file) ||
		    !esp_rcp_session_ready(adapter)) {
			result = -ENODEV;
			goto out_unlock;
		}

		ret = esp_rcp_send_chunk(file, adapter, buf + done, chunk);
		if (ret) {
			/*
			 * Once any bytes have been published, return the partial count
			 * so userspace never retries already-sent Spinel bytes.
			 */
			result = done ? done : ret;
			goto out_unlock;
		}
		done += chunk;
	}
	result = done;

out_unlock:
	mutex_unlock(&rcp_write_lock);
	return result;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0))
static __poll_t esp_rcp_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &rcp_read_wait, wait);
	poll_wait(file, &rcp_write_wait, wait);
	if (!esp_rcp_file_is_current(file))
		return EPOLLERR | EPOLLHUP;
	if (!skb_queue_empty(&rcp_rx_q))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (atomic_read(&rcp_tx_inflight) < ESP_RCP_TX_INFLIGHT_LIMIT)
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}
#else
static unsigned int esp_rcp_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;

	poll_wait(file, &rcp_read_wait, wait);
	poll_wait(file, &rcp_write_wait, wait);
	if (!esp_rcp_file_is_current(file))
		return POLLERR | POLLHUP;
	if (!skb_queue_empty(&rcp_rx_q))
		mask |= POLLIN | POLLRDNORM;
	if (atomic_read(&rcp_tx_inflight) < ESP_RCP_TX_INFLIGHT_LIMIT)
		mask |= POLLOUT | POLLWRNORM;
	return mask;
}
#endif

static const struct file_operations esp_rcp_fops = {
	.owner = THIS_MODULE,
	.open = esp_rcp_open,
	.release = esp_rcp_release,
	.read = esp_rcp_read,
	.write = esp_rcp_write,
	.poll = esp_rcp_poll,
};

static struct miscdevice esp_rcp_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = ESP_RCP_DEVICE_NAME,
	.fops = &esp_rcp_fops,
};

void esp_rcp_note_boot_event(struct esp_adapter *adapter, bool active_on_boot)
{
	bool invalidate_old_session = false;

	if (!adapter)
		return;

	/*
	 * A validated boot event is the generation boundary. Invalidate the old
	 * userspace generation synchronously, before any later RCP frame from the
	 * new firmware incarnation can be processed by the RX worker.
	 */
	mutex_lock(&rcp_lifecycle_lock);
	mutex_lock(&rcp_lock);
	esp_rcp_init_queues_locked();
	skb_queue_purge(&rcp_early_q);
	/*
	 * New runtime-control firmware advertises RCP support while inactive;
	 * legacy RCP firmware starts the backend before its boot event and may
	 * emit Spinel immediately. Preserve early-byte quarantine only for the
	 * latter while invalidating any old userspace generation in both cases.
	 */
	rcp_boot_adapter = active_on_boot ? adapter : NULL;
	rcp_boot_pending = active_on_boot;
	rcp_early_overflow = false;
	rcp_ever_opened = false;
	rcp_opening_first = false;
	rcp_session_acked_nonce = 0;

	if (rcp_registered && rcp_adapter == adapter &&
	    atomic_read(&rcp_session_active)) {
		atomic_set(&rcp_session_active, 0);
		rcp_opened = false;
		skb_queue_purge(&rcp_rx_q);
		invalidate_old_session = true;
	}
	mutex_unlock(&rcp_lock);

	/*
	 * Purge Host->ESP bytes from the previous Spinel generation while
	 * lifecycle serialization prevents close/reopen from racing the purge.
	 * In-flight stateful ownership is handled by the transport-specific flush.
	 */
	if (invalidate_old_session && adapter->if_ops &&
	    adapter->if_ops->flush_rcp_traffic)
		adapter->if_ops->flush_rcp_traffic(adapter);

	mutex_unlock(&rcp_lifecycle_lock);

	if (invalidate_old_session) {
		wake_up_interruptible(&rcp_read_wait);
		wake_up_interruptible(&rcp_write_wait);
	}
}

void esp_rcp_session_ack(struct esp_adapter *adapter,
			 const uint8_t *data, size_t len)
{
	const struct esp_rcp_session_marker *marker;
	u64 nonce_le;
	u64 nonce;
	bool accepted = false;

	if (!adapter || !data || len != sizeof(*marker))
		return;

	marker = (const struct esp_rcp_session_marker *)data;
	if (marker->version != ESP_RCP_SESSION_VERSION)
		return;

	memcpy(&nonce_le, &marker->nonce, sizeof(nonce_le));
	nonce = esp_wire_le64_to_cpu(nonce_le);

	mutex_lock(&rcp_lock);
	if (rcp_registered && rcp_opened &&
	    rcp_adapter == adapter &&
	    atomic_read(&rcp_session_active) &&
	    nonce && nonce == adapter->rcp_session_nonce &&
	    rcp_session_acked_nonce != nonce) {
		struct sk_buff *skb;

		rcp_session_acked_nonce = nonce;
		if (rcp_opening_first) {
			while ((skb = skb_dequeue(&rcp_early_q)) != NULL)
				skb_queue_tail(&rcp_rx_q, skb);
		} else {
			/* Reopen must never inherit pre-ACK DATA. */
			skb_queue_purge(&rcp_rx_q);
			skb_queue_purge(&rcp_early_q);
		}
		rcp_ever_opened = true;
		rcp_opening_first = false;
		accepted = true;
	}
	mutex_unlock(&rcp_lock);

	if (accepted) {
		wake_up(&rcp_session_wait);
		wake_up_interruptible(&rcp_read_wait);
	}
}

bool esp_rcp_fail_closed_required(struct esp_adapter *adapter)
{
	bool required;

	if (!adapter)
		return false;

	mutex_lock(&rcp_lock);
	required = (rcp_boot_pending && rcp_boot_adapter == adapter) ||
		   (rcp_registered && rcp_adapter == adapter &&
		    atomic_read(&rcp_session_active));
	mutex_unlock(&rcp_lock);
	return required;
}

static int esp_rcp_send_session_start(struct esp_adapter *adapter)
{
	struct esp_rcp_session_marker *marker;
	struct esp_payload_header *hdr;
	struct sk_buff *skb;
	u64 nonce_le;
	u8 pad_len;
	size_t payload_len = sizeof(*marker);
	size_t total_len;
	int ret;

	if (!adapter || !adapter->rcp_session_nonce)
		return -EINVAL;

	total_len = payload_len + sizeof(*hdr);
	pad_len = sizeof(*hdr);
	pad_len += (SKB_DATA_ADDR_ALIGNMENT -
		    (total_len % SKB_DATA_ADDR_ALIGNMENT)) %
		   SKB_DATA_ADDR_ALIGNMENT;

	skb = esp_if_alloc_skb(adapter, payload_len + pad_len);
	if (!skb)
		return -ENOMEM;

	skb_put(skb, payload_len + pad_len);
	memset(skb->data, 0, payload_len + pad_len);
	hdr = (struct esp_payload_header *)skb->data;
	hdr->if_type = ESP_RCP_IF;
	hdr->if_num = 0;
	hdr->len = esp_wire_cpu_to_le16(payload_len);
	hdr->offset = esp_wire_cpu_to_le16(pad_len);
	hdr->packet_type = PACKET_TYPE_RCP_SESSION;

	marker = (struct esp_rcp_session_marker *)(skb->data + pad_len);
	marker->version = ESP_RCP_SESSION_VERSION;
	nonce_le = esp_wire_cpu_to_le64(adapter->rcp_session_nonce);
	memcpy(&marker->nonce, &nonce_le, sizeof(nonce_le));

	if (adapter->capabilities & ESP_CHECKSUM_ENABLED)
		hdr->checksum = esp_wire_cpu_to_le16(
			compute_checksum(skb->data, payload_len + pad_len));

	ret = esp_send_packet(adapter, skb);
	if (ret)
		esp_err("Failed to publish RCP session-start marker: %d\n", ret);
	return ret;
}

int esp_init_rcp(struct esp_adapter *adapter)
{
	int ret;

	if (!adapter)
		return 0;

	mutex_lock(&rcp_lifecycle_lock);
	mutex_lock(&rcp_lock);
	esp_rcp_init_queues_locked();

	if (!(adapter->ext_capabilities & ESP_EXT_CAP_RCP_ANY)) {
		skb_queue_purge(&rcp_early_q);
		rcp_boot_pending = false;
		rcp_early_overflow = false;
		rcp_boot_adapter = NULL;
		mutex_unlock(&rcp_lock);
		mutex_unlock(&rcp_lifecycle_lock);
		return 0;
	}
	if (!adapter->rcp_session_nonce) {
		esp_err("RCP activation completed without a session nonce\n");
		skb_queue_purge(&rcp_early_q);
		rcp_boot_pending = false;
		rcp_early_overflow = false;
		rcp_boot_adapter = NULL;
		mutex_unlock(&rcp_lock);
		mutex_unlock(&rcp_lifecycle_lock);
		return -EPROTO;
	}
	if (rcp_registered) {
		ret = (rcp_adapter == adapter) ? 0 : -EBUSY;
		mutex_unlock(&rcp_lock);
		mutex_unlock(&rcp_lifecycle_lock);
		return ret;
	}

	rcp_adapter = adapter;
	rcp_opened = false;
	rcp_ever_opened = false;
	rcp_opening_first = false;
	/*
	 * Generation zero is reserved for never-opened contexts. Increment on
	 * every advertised firmware incarnation so an FD surviving misc_deregister
	 * can never become valid again after re-registration.
	 */
	rcp_generation++;
	if (!rcp_generation)
		rcp_generation++;
	skb_queue_purge(&rcp_rx_q);
	if (rcp_boot_pending && rcp_boot_adapter == adapter &&
	    rcp_early_overflow) {
		esp_err("RCP early RX overflow before session publication\n");
		skb_queue_purge(&rcp_early_q);
		rcp_boot_pending = false;
		rcp_early_overflow = false;
		rcp_boot_adapter = NULL;
		rcp_adapter = NULL;
		mutex_unlock(&rcp_lock);
		mutex_unlock(&rcp_lifecycle_lock);
		return -EOVERFLOW;
	}
	/*
	 * Keep one-time boot/startup bytes quarantined until the first open's
	 * nonce ACK. They are never copied directly into the readable queue.
	 */
	rcp_boot_pending = false;
	rcp_early_overflow = false;
	rcp_boot_adapter = NULL;

	/*
	 * Serialize misc registration with deinit. This closes the window where
	 * recovery could tear down the adapter after state publication but before
	 * /dev/esp_rcp0 became fully registered.
	 */
	ret = misc_register(&esp_rcp_miscdev);
	if (ret) {
		rcp_adapter = NULL;
		mutex_unlock(&rcp_lock);
		mutex_unlock(&rcp_lifecycle_lock);
		return ret;
	}

	rcp_registered = true;
	atomic_set(&rcp_session_active, 1);
	mutex_unlock(&rcp_lock);
	mutex_unlock(&rcp_lifecycle_lock);
	wake_up_interruptible(&rcp_read_wait);
	wake_up_interruptible(&rcp_write_wait);
	esp_info("RCP byte-stream endpoint registered as /dev/%s generation=%u\n",
		 ESP_RCP_DEVICE_NAME, rcp_generation);
	return 0;
}

void esp_deinit_rcp(struct esp_adapter *adapter)
{
	bool final_teardown;
	bool unregister;

	mutex_lock(&rcp_lifecycle_lock);
	mutex_lock(&rcp_lock);
	esp_rcp_init_queues_locked();
	final_teardown = !adapter ||
		(adapter && (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
			     test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags)));
	unregister = rcp_registered &&
		     (!adapter || !rcp_adapter || rcp_adapter == adapter);
	if (!unregister) {
		if (final_teardown) {
			skb_queue_purge(&rcp_early_q);
			rcp_boot_pending = false;
			rcp_early_overflow = false;
			rcp_boot_adapter = NULL;
		}
		mutex_unlock(&rcp_lock);
		mutex_unlock(&rcp_lifecycle_lock);
		return;
	}

	atomic_set(&rcp_session_active, 0);
	rcp_opened = false;
	rcp_ever_opened = false;
	rcp_opening_first = false;
	rcp_session_acked_nonce = 0;
	rcp_adapter = NULL;
	skb_queue_purge(&rcp_rx_q);
	if (final_teardown) {
		skb_queue_purge(&rcp_early_q);
		rcp_boot_pending = false;
		rcp_early_overflow = false;
		rcp_boot_adapter = NULL;
	}

	/*
	 * Keep lifecycle serialization through misc_deregister so a recovery boot
	 * cannot race a new misc_register against the old device teardown.
	 */
	misc_deregister(&esp_rcp_miscdev);
	rcp_registered = false;
	mutex_unlock(&rcp_lock);
	mutex_unlock(&rcp_lifecycle_lock);

	wake_up_interruptible(&rcp_read_wait);
	wake_up_interruptible(&rcp_write_wait);
	wake_up(&rcp_session_wait);
}

void esp_rcp_rx(struct esp_adapter *adapter, struct sk_buff *skb)
{
	bool overflow = false;
	bool opened = false;

	if (!skb)
		return;

	mutex_lock(&rcp_lock);
	esp_rcp_init_queues_locked();

	/*
	 * A boot event marks a new firmware incarnation before reconstruction has
	 * necessarily finished. Preserve only bytes ordered after that boot event
	 * in the early queue; they are transferred by esp_init_rcp().
	 */
	if (rcp_boot_pending && rcp_boot_adapter == adapter) {
		if (skb_queue_len(&rcp_early_q) >= ESP_RCP_RX_QUEUE_LIMIT) {
			rcp_early_overflow = true;
			skb_queue_purge(&rcp_early_q);
			mutex_unlock(&rcp_lock);
			dev_kfree_skb_any(skb);
			return;
		}
		skb_queue_tail(&rcp_early_q, skb);
		mutex_unlock(&rcp_lock);
		return;
	}

	if (!rcp_registered || rcp_adapter != adapter ||
	    !atomic_read(&rcp_session_active) ||
	    !esp_rcp_session_ready(adapter)) {
		mutex_unlock(&rcp_lock);
		dev_kfree_skb_any(skb);
		return;
	}

	opened = rcp_opened;
	if (!opened) {
		/*
		 * Once the first userspace session has closed, late firmware DATA is
		 * stale by definition. Do not let it refill the queue for a later
		 * opener that reuses this firmware incarnation's boot nonce.
		 */
		mutex_unlock(&rcp_lock);
		dev_kfree_skb_any(skb);
		return;
	}

	if (rcp_session_acked_nonce != adapter->rcp_session_nonce) {
		if (!rcp_opening_first) {
			/* Reopen: the ACK is the strict new-session DATA boundary. */
			mutex_unlock(&rcp_lock);
			dev_kfree_skb_any(skb);
			return;
		}

		/*
		 * First open only: preserve the RCP's one-time RESET/status which the
		 * firmware releases immediately before its first nonce ACK.
		 */
		if (skb_queue_len(&rcp_early_q) >= ESP_RCP_RX_QUEUE_LIMIT) {
			overflow = true;
			skb_queue_purge(&rcp_early_q);
			mutex_unlock(&rcp_lock);
			dev_kfree_skb_any(skb);
		} else {
			skb_queue_tail(&rcp_early_q, skb);
			mutex_unlock(&rcp_lock);
			return;
		}
	} else if (skb_queue_len(&rcp_rx_q) >= ESP_RCP_RX_QUEUE_LIMIT) {
		overflow = true;
		skb_queue_purge(&rcp_rx_q);
		mutex_unlock(&rcp_lock);
		dev_kfree_skb_any(skb);
	} else {
		skb_queue_tail(&rcp_rx_q, skb);
		mutex_unlock(&rcp_lock);
		wake_up_interruptible(&rcp_read_wait);
		return;
	}

	if (overflow) {
		esp_err("RCP RX queue overflow; resetting stateful session\n");
		esp_schedule_fw_reset_recovery(adapter);
		esp_request_firmware_restart(adapter);
		wake_up_interruptible(&rcp_read_wait);
	}
}
