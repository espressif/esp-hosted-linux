// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESP32-S31 USB transport backend for the cfg80211 ESP-Hosted driver.
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

#include "utils.h"
#include "esp_if.h"
#include "esp_api.h"
#include "esp_kernel_port.h"
#include "esp_utils.h"
#include "esp_rcp_api.h"

#define ESP_USB_VENDOR_ID              0x303a
#define ESP_USB_PRODUCT_ID_S31         0x4002
#define ESP_USB_MAX_XFER               (16 * 1024)
#define ESP_USB_RX_URBS                8
#define ESP_USB_TX_URBS                4
#define ESP_USB_TX_QUEUE_MAX           256
#define ESP_USB_CTRL_TIMEOUT_MS        1000

#define ESP_USB_VENDOR_REQ_READY_REPLAY 0xA0
#define ESP_USB_VENDOR_REQ_SOFT_RESET   0xA1
#define ESP_USB_VENDOR_STATUS_OK        0
#define ESP_USB_VENDOR_STATUS_NOT_READY 1

struct esp_usb_context;

struct esp_usb_rx_slot {
	struct esp_usb_context *ctx;
	struct urb *urb;
	u8 *buf;
};

struct esp_usb_tx_ctx {
	struct esp_usb_context *ctx;
	struct sk_buff *skb;
	u8 if_type;
};

struct esp_usb_context {
	struct usb_interface *intf;
	struct usb_device *udev;
	struct esp_adapter *adapter;
	u8 bulk_in_addr;
	u8 bulk_out_addr;
	u16 bulk_in_mps;
	u16 bulk_out_mps;
	bool high_speed;
	bool running;
	bool tx_quiesced;
	bool flush_in_progress;
	u8 flush_if_type;
	spinlock_t tx_state_lock;
	struct mutex tx_flush_lock;

	struct usb_anchor rx_anchor;
	struct usb_anchor tx_anchor;
	struct esp_usb_rx_slot rx_slot[ESP_USB_RX_URBS];

	struct sk_buff_head rx_queue;
	struct sk_buff_head tx_queue[MAX_PRIORITY_QUEUES];
	struct workqueue_struct *tx_wq;
	struct work_struct tx_work;
	struct work_struct stateful_fail_work;
	atomic_t tx_inflight;
	atomic_t rcp_tx_inflight;
	atomic_t hci_tx_inflight;
	atomic_t stateful_fail_force;

	spinlock_t rx_lock;
	u8 *rx_stream;
	size_t rx_stream_len;
	size_t rx_stream_cap;
	bool rx_pad_expected;

};

static struct esp_adapter *s_adapter;
volatile u8 host_sleep;

static void esp_usb_stateful_fail_work(struct work_struct *work)
{
	struct esp_usb_context *ctx =
		container_of(work, struct esp_usb_context, stateful_fail_work);
	struct esp_adapter *adapter = ctx->adapter;
	bool force_reset = atomic_xchg(&ctx->stateful_fail_force, 0);

	if (!ctx->running || !adapter ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return;

	if (!force_reset && !esp_rcp_fail_closed_required(adapter)) {
		esp_schedule_transport_recovery(adapter);
		return;
	}

	dev_err(&ctx->intf->dev,
		"USB stateful stream delivery became ambiguous; resetting firmware\n");
	esp_schedule_fw_reset_recovery(adapter);
	esp_request_firmware_restart(adapter);
}

static void esp_usb_schedule_stateful_fail(struct esp_usb_context *ctx,
					    bool force_reset)
{
	struct esp_adapter *adapter;

	if (!ctx || !ctx->running)
		return;

	adapter = ctx->adapter;
	if (!adapter ||
	    test_bit(ESP_FW_RECOVERY_PENDING, &adapter->state_flags) ||
	    test_bit(ESP_FW_RESET_EXPECTED, &adapter->state_flags) ||
	    test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) ||
	    test_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags))
		return;

	if (force_reset)
		atomic_set(&ctx->stateful_fail_force, 1);
	schedule_work(&ctx->stateful_fail_work);
}

static u8 esp_usb_skb_if_type(const struct sk_buff *skb)
{
	const struct esp_payload_header *hdr;

	if (!skb || skb->len < sizeof(*hdr))
		return ESP_MAX_IF;
	hdr = (const struct esp_payload_header *)skb->data;
	return hdr->if_type < ESP_MAX_IF ? hdr->if_type : ESP_MAX_IF;
}

static int esp_usb_vendor_request(struct esp_usb_context *ctx, u8 request)
{
	__le32 *status;
	u8 ifnum;
	u32 value;
	int ret;

	if (!ctx || !ctx->udev || !ctx->intf || !ctx->intf->cur_altsetting)
		return -ENODEV;

	status = kmalloc(sizeof(*status), GFP_KERNEL);
	if (!status)
		return -ENOMEM;
	*status = cpu_to_le32(ESP_USB_VENDOR_STATUS_NOT_READY);

	ifnum = ctx->intf->cur_altsetting->desc.bInterfaceNumber;
	ret = usb_control_msg(ctx->udev, usb_rcvctrlpipe(ctx->udev, 0),
			      request,
			      USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
			      0, ifnum, status, sizeof(*status),
			      ESP_USB_CTRL_TIMEOUT_MS);
	if (ret < 0)
		goto out;
	if (ret != sizeof(*status)) {
		ret = -EIO;
		goto out;
	}

	value = le32_to_cpu(*status);
	ret = value == ESP_USB_VENDOR_STATUS_OK ? 0 : -EIO;
out:
	kfree(status);
	return ret;
}

static bool esp_usb_valid_header(const struct esp_usb_context *ctx,
				 const struct esp_payload_header *hdr,
				 size_t *frame_len)
{
	u16 offset;
	u16 payload_len;

	if (!ctx || !hdr || hdr->if_type >= ESP_MAX_IF)
		return false;

	offset = esp_wire_le16_to_cpu(hdr->offset);
	payload_len = esp_wire_le16_to_cpu(hdr->len);
	if (!payload_len || !ESP_OFFSET_VALID(offset))
		return false;

	*frame_len = (size_t)offset + payload_len;
	return *frame_len <= ESP_USB_MAX_XFER;
}

static bool esp_usb_queue_frame_locked(struct esp_usb_context *ctx,
				       const u8 *data, size_t len)
{
	struct sk_buff *skb;

	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb)
		return false;

	memcpy(skb_put(skb, len), data, len);
	skb_queue_tail(&ctx->rx_queue, skb);
	return true;
}

static bool esp_usb_parse_stream_locked(struct esp_usb_context *ctx,
					 bool *stream_loss)
{
	const struct esp_payload_header *hdr;
	size_t frame_len;
	size_t remaining;
	bool queued = false;

	for (;;) {
		/*
		 * Firmware deliberately emits one zero byte after a short logical
		 * frame whose length is an exact USB max-packet multiple. This avoids
		 * relying on ZLP termination for the vendor byte stream. The byte is
		 * transport framing, not Hosted data and therefore must not be treated
		 * as stateful stream corruption.
		 */
		if (ctx->rx_pad_expected && ctx->rx_stream_len) {
			if (ctx->rx_stream[0] == 0) {
				remaining = ctx->rx_stream_len - 1;
				if (remaining)
					memmove(ctx->rx_stream, ctx->rx_stream + 1,
						remaining);
				ctx->rx_stream_len = remaining;
			}
			ctx->rx_pad_expected = false;
		}

		if (ctx->rx_stream_len < sizeof(*hdr))
			break;

		hdr = (const struct esp_payload_header *)ctx->rx_stream;
		if (!esp_usb_valid_header(ctx, hdr, &frame_len)) {
			if (stream_loss)
				*stream_loss = true;
			memmove(ctx->rx_stream, ctx->rx_stream + 1,
				ctx->rx_stream_len - 1);
			ctx->rx_stream_len--;
			continue;
		}

		if (ctx->rx_stream_len < frame_len)
			break;

		if (esp_usb_queue_frame_locked(ctx, ctx->rx_stream, frame_len))
			queued = true;
		else if (stream_loss)
			*stream_loss = true;

		/*
		 * Mirror usb_write_all() on the S31 firmware. The device appends one
		 * pad byte only for a short logical frame ending exactly on its IN
		 * endpoint MPS boundary.
		 */
		ctx->rx_pad_expected =
			ctx->bulk_in_mps && frame_len < ESP_USB_MAX_XFER &&
			(frame_len % ctx->bulk_in_mps) == 0;

		remaining = ctx->rx_stream_len - frame_len;
		if (remaining)
			memmove(ctx->rx_stream, ctx->rx_stream + frame_len,
				remaining);
		ctx->rx_stream_len = remaining;
	}

	return queued;
}

static void esp_usb_ingest_stream(struct esp_usb_context *ctx,
				  const u8 *data, size_t len)
{
	unsigned long flags;
	size_t copy;
	bool queued = false;
	bool stream_loss = false;

	if (!ctx || !data || !len)
		return;

	spin_lock_irqsave(&ctx->rx_lock, flags);
	while (len) {
		if (ctx->rx_stream_len == ctx->rx_stream_cap) {
			ctx->rx_stream_len = 0;
			stream_loss = true;
		}

		copy = min(len, ctx->rx_stream_cap - ctx->rx_stream_len);
		memcpy(ctx->rx_stream + ctx->rx_stream_len, data, copy);
		ctx->rx_stream_len += copy;
		data += copy;
		len -= copy;
		queued |= esp_usb_parse_stream_locked(ctx, &stream_loss);
	}
	spin_unlock_irqrestore(&ctx->rx_lock, flags);

	if (stream_loss) {
		dev_warn_ratelimited(&ctx->intf->dev,
			"USB RX byte-stream framing loss detected\n");
		esp_usb_schedule_stateful_fail(ctx, true);
	}
	if (queued)
		esp_process_new_packet_intr(ctx->adapter);
}

static void esp_usb_rx_complete(struct urb *urb)
{
	struct esp_usb_rx_slot *slot = urb->context;
	struct esp_usb_context *ctx;
	int status;
	int ret;

	if (!slot || !slot->ctx)
		return;
	ctx = slot->ctx;
	status = urb->status;

	if (!ctx->running || status == -ENOENT || status == -ESHUTDOWN ||
	    status == -ECONNRESET)
		return;

	if (status) {
		dev_warn_ratelimited(&ctx->intf->dev,
			"USB RX URB failed: %d\n", status);
		esp_usb_schedule_stateful_fail(ctx, true);
	}
	if (!status && urb->actual_length > 0) {
		esp_usb_ingest_stream(ctx, slot->buf, urb->actual_length);
	}

	if (!ctx->running)
		return;

	usb_fill_bulk_urb(slot->urb, ctx->udev,
			  usb_rcvbulkpipe(ctx->udev, ctx->bulk_in_addr),
			  slot->buf, ESP_USB_MAX_XFER,
			  esp_usb_rx_complete, slot);
	usb_anchor_urb(slot->urb, &ctx->rx_anchor);
	ret = usb_submit_urb(slot->urb, GFP_ATOMIC);
	if (ret) {
		usb_unanchor_urb(slot->urb);
		if (ret != -ENODEV && ret != -ESHUTDOWN) {
			dev_err(&ctx->intf->dev, "USB RX resubmit failed: %d\n", ret);
			esp_usb_schedule_stateful_fail(ctx, true);
		}
	}
}

static int esp_usb_start_rx(struct esp_usb_context *ctx)
{
	int i;
	int ret;

	for (i = 0; i < ESP_USB_RX_URBS; i++) {
		struct esp_usb_rx_slot *slot = &ctx->rx_slot[i];

		slot->ctx = ctx;
		slot->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!slot->urb)
			return -ENOMEM;
		slot->buf = kmalloc(ESP_USB_MAX_XFER, GFP_KERNEL);
		if (!slot->buf)
			return -ENOMEM;

		usb_fill_bulk_urb(slot->urb, ctx->udev,
				  usb_rcvbulkpipe(ctx->udev, ctx->bulk_in_addr),
				  slot->buf, ESP_USB_MAX_XFER,
				  esp_usb_rx_complete, slot);
		usb_anchor_urb(slot->urb, &ctx->rx_anchor);
		ret = usb_submit_urb(slot->urb, GFP_KERNEL);
		if (ret) {
			usb_unanchor_urb(slot->urb);
			return ret;
		}
	}

	return 0;
}

static void esp_usb_free_rx_slots(struct esp_usb_context *ctx)
{
	int i;

	for (i = 0; i < ESP_USB_RX_URBS; i++) {
		if (ctx->rx_slot[i].urb)
			usb_free_urb(ctx->rx_slot[i].urb);
		kfree(ctx->rx_slot[i].buf);
		ctx->rx_slot[i].urb = NULL;
		ctx->rx_slot[i].buf = NULL;
		ctx->rx_slot[i].ctx = NULL;
	}
}

static void esp_usb_tx_complete(struct urb *urb)
{
	struct esp_usb_tx_ctx *tx = urb->context;
	struct esp_usb_context *ctx;

	if (!tx)
		goto out_free_urb;
	ctx = tx->ctx;

	if (ctx) {
		if (urb->status && !ctx->tx_quiesced &&
		    (tx->if_type == ESP_RCP_IF || tx->if_type == ESP_HCI_IF)) {
			dev_warn_ratelimited(&ctx->intf->dev,
				"USB stateful TX URB failed: if=%u status=%d\n",
				tx->if_type, urb->status);
			esp_usb_schedule_stateful_fail(ctx, true);
		}
		if (tx->if_type == ESP_RCP_IF)
			atomic_dec(&ctx->rcp_tx_inflight);
		else if (tx->if_type == ESP_HCI_IF)
			atomic_dec(&ctx->hci_tx_inflight);
		atomic_dec(&ctx->tx_inflight);
		if (ctx->tx_wq) {
			unsigned long flags;

			/*
			 * Serialize the requeue decision with flush entry. A completion
			 * that races cancel_work_sync() must not recreate TX work after
			 * the flush boundary has been established.
			 */
			spin_lock_irqsave(&ctx->tx_state_lock, flags);
			if (ctx->running && !ctx->tx_quiesced &&
			    !ctx->flush_in_progress)
				queue_work(ctx->tx_wq, &ctx->tx_work);
			spin_unlock_irqrestore(&ctx->tx_state_lock, flags);
		}
	}

	dev_kfree_skb_any(tx->skb);
	kfree(tx);
out_free_urb:
	usb_free_urb(urb);
}

static int esp_usb_submit_tx(struct esp_usb_context *ctx, struct sk_buff *skb)
{
	struct esp_usb_tx_ctx *tx;
	struct urb *urb;
	int ret;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	tx = kzalloc(sizeof(*tx), GFP_KERNEL);
	if (!tx) {
		usb_free_urb(urb);
		return -ENOMEM;
	}
	tx->ctx = ctx;
	tx->skb = skb;
	tx->if_type = esp_usb_skb_if_type(skb);

	usb_fill_bulk_urb(urb, ctx->udev,
			  usb_sndbulkpipe(ctx->udev, ctx->bulk_out_addr),
			  skb->data, skb->len, esp_usb_tx_complete, tx);
	urb->transfer_flags |= URB_ZERO_PACKET;

	atomic_inc(&ctx->tx_inflight);
	usb_anchor_urb(urb, &ctx->tx_anchor);
	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret) {
		usb_unanchor_urb(urb);
		atomic_dec(&ctx->tx_inflight);
		kfree(tx);
		usb_free_urb(urb);
		return ret;
	}

	return 0;
}

static u8 esp_usb_tx_priority(const struct sk_buff *skb)
{
	u8 if_type = esp_usb_skb_if_type(skb);

	if (if_type == ESP_INTERNAL_IF)
		return PRIO_Q_HIGH;
	if (if_type == ESP_HCI_IF || if_type == ESP_RCP_IF)
		return PRIO_Q_MID;
	return PRIO_Q_LOW;
}

static unsigned int esp_usb_tx_queue_len(struct esp_usb_context *ctx)
{
	unsigned int len = 0;
	u8 q;

	for (q = 0; q < MAX_PRIORITY_QUEUES; q++)
		len += skb_queue_len(&ctx->tx_queue[q]);
	return len;
}

static struct sk_buff *esp_usb_dequeue_tx(struct esp_usb_context *ctx)
{
	struct sk_buff *skb;
	u8 q;

	for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
		skb = skb_dequeue(&ctx->tx_queue[q]);
		if (skb)
			return skb;
	}
	return NULL;
}

static void esp_usb_tx_work(struct work_struct *work)
{
	struct esp_usb_context *ctx =
		container_of(work, struct esp_usb_context, tx_work);
	struct sk_buff *skb;
	int ret;

	for (;;) {
		unsigned long flags;
		u8 if_type;

		spin_lock_irqsave(&ctx->tx_state_lock, flags);
		if (!ctx->running || ctx->tx_quiesced || ctx->flush_in_progress ||
		    atomic_read(&ctx->tx_inflight) >= ESP_USB_TX_URBS) {
			spin_unlock_irqrestore(&ctx->tx_state_lock, flags);
			break;
		}
		skb = esp_usb_dequeue_tx(ctx);
		if (!skb) {
			spin_unlock_irqrestore(&ctx->tx_state_lock, flags);
			break;
		}

		/*
		 * Count stateful ownership before dropping tx_state_lock. A flush
		 * entering now can therefore see a packet already removed from the
		 * software queue even if usb_submit_urb() has not happened yet.
		 */
		if_type = esp_usb_skb_if_type(skb);
		if (if_type == ESP_RCP_IF)
			atomic_inc(&ctx->rcp_tx_inflight);
		else if (if_type == ESP_HCI_IF)
			atomic_inc(&ctx->hci_tx_inflight);
		spin_unlock_irqrestore(&ctx->tx_state_lock, flags);

		ret = esp_usb_submit_tx(ctx, skb);
		if (ret) {
			if (if_type == ESP_RCP_IF)
				atomic_dec(&ctx->rcp_tx_inflight);
			else if (if_type == ESP_HCI_IF)
				atomic_dec(&ctx->hci_tx_inflight);
			dev_kfree_skb_any(skb);
			if (ret != -ENODEV && ret != -ESHUTDOWN) {
				dev_err(&ctx->intf->dev,
					"USB TX submit failed: %d\n", ret);
				esp_schedule_transport_recovery(ctx->adapter);
			}
			break;
		}
	}
}

static struct sk_buff *esp_usb_read(struct esp_adapter *adapter)
{
	struct esp_usb_context *ctx;

	if (!adapter)
		return NULL;
	ctx = adapter->if_context;
	if (!ctx)
		return NULL;

	return skb_dequeue(&ctx->rx_queue);
}

static int esp_usb_write(struct esp_adapter *adapter, struct sk_buff *skb)
{
	struct esp_usb_context *ctx;
	unsigned long flags;
	u8 if_type;
	u8 priority;
	int ret = 0;

	if (!skb)
		return -EINVAL;
	if (!adapter || !(ctx = adapter->if_context) || !ctx->bulk_out_addr) {
		dev_kfree_skb_any(skb);
		return -ENODEV;
	}

	if (skb->len > ESP_USB_MAX_XFER) {
		dev_kfree_skb_any(skb);
		return -EMSGSIZE;
	}

	if (skb_is_nonlinear(skb) && skb_linearize(skb)) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}

	if_type = esp_usb_skb_if_type(skb);
	priority = esp_usb_tx_priority(skb);

	/*
	 * The state check, queue admission and work scheduling decision are one
	 * atomic transition with respect to stateful flush. In particular, no
	 * RCP/HCI skb can enter its queue after that interface's flush boundary.
	 */
	spin_lock_irqsave(&ctx->tx_state_lock, flags);
	if (!ctx->running || ctx->tx_quiesced) {
		ret = -ENODEV;
	} else if (ctx->flush_in_progress && ctx->flush_if_type == if_type) {
		ret = -EBUSY;
	} else if (esp_usb_tx_queue_len(ctx) >= ESP_USB_TX_QUEUE_MAX) {
		ret = -EBUSY;
	} else {
		skb_queue_tail(&ctx->tx_queue[priority], skb);
		if (!ctx->flush_in_progress && ctx->tx_wq)
			queue_work(ctx->tx_wq, &ctx->tx_work);
	}
	spin_unlock_irqrestore(&ctx->tx_state_lock, flags);

	if (ret)
		dev_kfree_skb_any(skb);
	return ret;
}

static struct sk_buff *esp_usb_alloc_skb(u32 len)
{
	struct sk_buff *skb;
	u32 offset;
	u32 alloc_len;

	/*
	 * Keep the transport headroom and align the data pointer. Common TX
	 * code does not re-check alignment after calling the transport
	 * allocator.
	 */
	alloc_len = len + INTERFACE_HEADER_PADDING + SKB_DATA_ADDR_ALIGNMENT;
	skb = netdev_alloc_skb(NULL, alloc_len);
	if (!skb)
		return NULL;

	skb_reserve(skb, INTERFACE_HEADER_PADDING);
	offset = ((unsigned long)skb->data) & (SKB_DATA_ADDR_ALIGNMENT - 1);
	if (offset)
		skb_reserve(skb, SKB_DATA_ADDR_ALIGNMENT - offset);

	return skb;
}

static void esp_usb_reset_rx_stream(struct esp_usb_context *ctx)
{
	unsigned long flags;

	if (!ctx)
		return;

	spin_lock_irqsave(&ctx->rx_lock, flags);
	ctx->rx_stream_len = 0;
	ctx->rx_pad_expected = false;
	spin_unlock_irqrestore(&ctx->rx_lock, flags);
}

static int esp_usb_rearm_rx(struct esp_usb_context *ctx)
{
	int i;
	int ret;

	if (!ctx || !ctx->running)
		return -ENODEV;

	/*
	 * A failed completion-time resubmit leaves that persistent RX slot idle.
	 * Kill all slots first, then re-arm all RX slots so recovery cannot
	 * gradually degrade to fewer URBs.
	 */
	for (i = 0; i < ESP_USB_RX_URBS; i++) {
		if (ctx->rx_slot[i].urb)
			usb_kill_urb(ctx->rx_slot[i].urb);
	}
	esp_usb_reset_rx_stream(ctx);

	for (i = 0; i < ESP_USB_RX_URBS; i++) {
		struct esp_usb_rx_slot *slot = &ctx->rx_slot[i];

		if (!slot->urb || !slot->buf)
			return -ENODEV;

		usb_fill_bulk_urb(slot->urb, ctx->udev,
				  usb_rcvbulkpipe(ctx->udev, ctx->bulk_in_addr),
				  slot->buf, ESP_USB_MAX_XFER,
				  esp_usb_rx_complete, slot);
		usb_anchor_urb(slot->urb, &ctx->rx_anchor);
		ret = usb_submit_urb(slot->urb, GFP_KERNEL);
		if (ret) {
			usb_unanchor_urb(slot->urb);
			while (--i >= 0)
				usb_kill_urb(ctx->rx_slot[i].urb);
			return ret;
		}
	}

	return 0;
}

static int esp_usb_quiesce_for_fw_reset(struct esp_adapter *adapter)
{
	struct esp_usb_context *ctx;
	int i;

	if (!adapter || !(ctx = adapter->if_context))
		return -ENODEV;

	{
		unsigned long flags;

		spin_lock_irqsave(&ctx->tx_state_lock, flags);
		ctx->tx_quiesced = true;
		spin_unlock_irqrestore(&ctx->tx_state_lock, flags);
	}
	if (ctx->tx_wq)
		cancel_work_sync(&ctx->tx_work);
	for (i = 0; i < MAX_PRIORITY_QUEUES; i++)
		skb_queue_purge(&ctx->tx_queue[i]);
	usb_kill_anchored_urbs(&ctx->tx_anchor);
	atomic_set(&ctx->tx_inflight, 0);
	atomic_set(&ctx->rcp_tx_inflight, 0);
	atomic_set(&ctx->hci_tx_inflight, 0);

	/*
	 * Do not carry partial bytes or completed frames across firmware
	 * incarnations. The boot event that triggered this path has already been
	 * dequeued by the common RX worker.
	 */
	for (i = 0; i < ESP_USB_RX_URBS; i++) {
		if (ctx->rx_slot[i].urb)
			usb_kill_urb(ctx->rx_slot[i].urb);
	}
	esp_usb_reset_rx_stream(ctx);
	skb_queue_purge(&ctx->rx_queue);
	return 0;
}

static int esp_usb_reinit_after_fw_reset(struct esp_adapter *adapter)
{
	struct esp_usb_context *ctx;
	int ret;

	if (!adapter || !(ctx = adapter->if_context) || !ctx->running)
		return -ENODEV;

	ret = esp_usb_rearm_rx(ctx);
	if (ret)
		return ret;

	{
		unsigned long flags;

		spin_lock_irqsave(&ctx->tx_state_lock, flags);
		ctx->tx_quiesced = false;
		if (ctx->tx_wq)
			queue_work(ctx->tx_wq, &ctx->tx_work);
		spin_unlock_irqrestore(&ctx->tx_state_lock, flags);
	}
	return 0;
}

static int esp_usb_recover_transport(struct esp_adapter *adapter)
{
	struct esp_usb_context *ctx;
	int ret;

	if (!adapter || !(ctx = adapter->if_context) || !ctx->running)
		return -ENODEV;

	/*
	 * Re-arm every persistent RX slot before asking firmware to replay READY.
	 * This also repairs a slot lost to a transient completion-time resubmit
	 * failure and guarantees the READY event has a live receive path. Drop any
	 * queued pre-recovery frames so the replay starts from a clean boundary.
	 */
	skb_queue_purge(&ctx->rx_queue);
	ret = esp_usb_rearm_rx(ctx);
	if (ret)
		return ret;

	/* READY replay publishes a fresh boot TLV asynchronously. Return zero so
	 * the common recovery state machine waits for that event. */
	return esp_usb_vendor_request(ctx, ESP_USB_VENDOR_REQ_READY_REPLAY);
}

static void esp_usb_flush_if_traffic(struct esp_adapter *adapter, u8 if_type)
{
	struct esp_usb_context *ctx;
	struct sk_buff_head keep;
	struct sk_buff *skb;
	unsigned long flags;
	bool inflight = false;
	u8 q;

	if (!adapter || !(ctx = adapter->if_context))
		return;

	/*
	 * Only one stateful flush may own the exclusion boundary at a time.
	 * tx_state_lock then serializes that boundary with write(), TX work and
	 * completion-time work requeue.
	 */
	mutex_lock(&ctx->tx_flush_lock);
	spin_lock_irqsave(&ctx->tx_state_lock, flags);
	ctx->flush_in_progress = true;
	ctx->flush_if_type = if_type;
	if (if_type == ESP_RCP_IF)
		inflight = atomic_read(&ctx->rcp_tx_inflight) != 0;
	else if (if_type == ESP_HCI_IF)
		inflight = atomic_read(&ctx->hci_tx_inflight) != 0;
	spin_unlock_irqrestore(&ctx->tx_state_lock, flags);

	if (ctx->tx_wq)
		cancel_work_sync(&ctx->tx_work);

	skb_queue_head_init(&keep);
	spin_lock_irqsave(&ctx->tx_state_lock, flags);
	for (q = 0; q < MAX_PRIORITY_QUEUES; q++) {
		while ((skb = skb_dequeue(&ctx->tx_queue[q])) != NULL) {
			if (esp_usb_skb_if_type(skb) == if_type)
				dev_kfree_skb_any(skb);
			else
				skb_queue_tail(&keep, skb);
		}
		while ((skb = skb_dequeue(&keep)) != NULL)
			skb_queue_tail(&ctx->tx_queue[esp_usb_tx_priority(skb)], skb);
	}

	if (if_type == ESP_RCP_IF)
		inflight |= atomic_read(&ctx->rcp_tx_inflight) != 0;
	else if (if_type == ESP_HCI_IF)
		inflight |= atomic_read(&ctx->hci_tx_inflight) != 0;

	ctx->flush_if_type = ESP_MAX_IF;
	ctx->flush_in_progress = false;
	if (ctx->running && !ctx->tx_quiesced && ctx->tx_wq)
		queue_work(ctx->tx_wq, &ctx->tx_work);
	spin_unlock_irqrestore(&ctx->tx_state_lock, flags);
	mutex_unlock(&ctx->tx_flush_lock);

	/*
	 * Once a stateful USB frame was submitted, completion cannot prove that
	 * userspace closing the stream raced before or after device consumption.
	 * Reincarnate firmware rather than let a later HCI/Spinel session inherit
	 * bytes from the old one.
	 */
	if (inflight)
		esp_usb_schedule_stateful_fail(ctx, true);
}

static void esp_usb_flush_bt_traffic(struct esp_adapter *adapter)
{
	esp_usb_flush_if_traffic(adapter, ESP_HCI_IF);
}

static void esp_usb_flush_rcp_traffic(struct esp_adapter *adapter)
{
	esp_usb_flush_if_traffic(adapter, ESP_RCP_IF);
}

static struct esp_if_ops esp_usb_if_ops = {
	.read = esp_usb_read,
	.write = esp_usb_write,
	.alloc_skb = esp_usb_alloc_skb,
	.quiesce_for_fw_reset = esp_usb_quiesce_for_fw_reset,
	.reinit_after_fw_reset = esp_usb_reinit_after_fw_reset,
	.recover_transport = esp_usb_recover_transport,
	.flush_bt_traffic = esp_usb_flush_bt_traffic,
	.flush_rcp_traffic = esp_usb_flush_rcp_traffic,
};

static int esp_usb_find_endpoints(struct esp_usb_context *ctx)
{
	struct usb_host_interface *alt;
	struct usb_endpoint_descriptor *ep;
	int i;

	alt = ctx->intf->cur_altsetting;
	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		ep = &alt->endpoint[i].desc;
		if (usb_endpoint_is_bulk_in(ep) && !ctx->bulk_in_addr) {
			ctx->bulk_in_addr = ep->bEndpointAddress;
			ctx->bulk_in_mps = usb_endpoint_maxp(ep);
		} else if (usb_endpoint_is_bulk_out(ep) && !ctx->bulk_out_addr) {
			ctx->bulk_out_addr = ep->bEndpointAddress;
			ctx->bulk_out_mps = usb_endpoint_maxp(ep);
		}
	}

	return ctx->bulk_in_addr && ctx->bulk_out_addr ? 0 : -ENODEV;
}

static void esp_usb_cleanup_context(struct esp_usb_context *ctx)
{
	int i;

	if (!ctx)
		return;

	{
		unsigned long flags;

		spin_lock_irqsave(&ctx->tx_state_lock, flags);
		ctx->running = false;
		ctx->tx_quiesced = true;
		spin_unlock_irqrestore(&ctx->tx_state_lock, flags);
	}
	if (ctx->tx_wq)
		cancel_work_sync(&ctx->tx_work);
	cancel_work_sync(&ctx->stateful_fail_work);

	/*
	 * RX URBs are persistent objects. Kill each one explicitly so a
	 * completion that has already been unanchored cannot race the slot/context
	 * teardown. Dynamic TX URBs remain anchor-owned until completion.
	 */
	for (i = 0; i < ESP_USB_RX_URBS; i++) {
		if (ctx->rx_slot[i].urb)
			usb_kill_urb(ctx->rx_slot[i].urb);
	}
	usb_kill_anchored_urbs(&ctx->tx_anchor);
	skb_queue_purge(&ctx->rx_queue);
	for (i = 0; i < MAX_PRIORITY_QUEUES; i++)
		skb_queue_purge(&ctx->tx_queue[i]);
	esp_usb_free_rx_slots(ctx);
	if (ctx->tx_wq)
		destroy_workqueue(ctx->tx_wq);
	kfree(ctx->rx_stream);
	usb_put_dev(ctx->udev);
	kfree(ctx);
}

static int esp_usb_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	struct esp_usb_context *ctx;
	struct usb_device *udev;
	struct esp_adapter *adapter = s_adapter;
	int ret;

	(void)id;
	if (!adapter)
		return -ENODEV;
	if (adapter->if_context)
		return -EBUSY;

	udev = interface_to_usbdev(intf);
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->intf = intf;
	ctx->udev = usb_get_dev(udev);
	ctx->adapter = adapter;
	init_usb_anchor(&ctx->rx_anchor);
	init_usb_anchor(&ctx->tx_anchor);
	skb_queue_head_init(&ctx->rx_queue);
	for (ret = 0; ret < MAX_PRIORITY_QUEUES; ret++)
		skb_queue_head_init(&ctx->tx_queue[ret]);
	spin_lock_init(&ctx->rx_lock);
	spin_lock_init(&ctx->tx_state_lock);
	mutex_init(&ctx->tx_flush_lock);
	ctx->flush_if_type = ESP_MAX_IF;
	atomic_set(&ctx->tx_inflight, 0);
	atomic_set(&ctx->rcp_tx_inflight, 0);
	atomic_set(&ctx->hci_tx_inflight, 0);
	atomic_set(&ctx->stateful_fail_force, 0);
	INIT_WORK(&ctx->tx_work, esp_usb_tx_work);
	INIT_WORK(&ctx->stateful_fail_work, esp_usb_stateful_fail_work);

	ctx->high_speed = udev->speed == USB_SPEED_HIGH ||
			  udev->speed == USB_SPEED_SUPER;
	ctx->rx_stream_cap = ESP_USB_MAX_XFER * 2;
	ctx->rx_stream = kzalloc(ctx->rx_stream_cap, GFP_KERNEL);
	if (!ctx->rx_stream) {
		ret = -ENOMEM;
		goto fail;
	}

	ctx->tx_wq = alloc_ordered_workqueue("ESP_USB_TX",
					     WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (!ctx->tx_wq) {
		ret = -ENOMEM;
		goto fail;
	}

	ret = esp_usb_find_endpoints(ctx);
	if (ret)
		goto fail;

	adapter->if_context = ctx;
	adapter->if_ops = &esp_usb_if_ops;
	adapter->if_type = ESP_IF_TYPE_USB;
	adapter->dev = &intf->dev;
	clear_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags);
	clear_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
	atomic_set(&adapter->state, ESP_CONTEXT_RX_READY);

	ctx->running = true;
	usb_set_intfdata(intf, ctx);

	ret = esp_usb_start_rx(ctx);
	if (ret)
		goto fail_bound;

	ret = esp_usb_vendor_request(ctx, ESP_USB_VENDOR_REQ_READY_REPLAY);
	if (ret)
		dev_dbg(&intf->dev,
			"USB READY replay request unavailable at probe: %d\n", ret);

	schedule_delayed_work(&adapter->fw_recovery_work,
			      msecs_to_jiffies(ESP_FW_RECOVERY_WATCHDOG_MS));

	dev_info(&intf->dev,
		 "ESP32-S31 USB transport bound (%s in=0x%02x/%u out=0x%02x/%u)\n",
		 ctx->high_speed ? "high-speed" : "full-speed",
		 ctx->bulk_in_addr, ctx->bulk_in_mps,
		 ctx->bulk_out_addr, ctx->bulk_out_mps);
	return 0;

fail_bound:
	usb_set_intfdata(intf, NULL);
	adapter->if_context = NULL;
	adapter->dev = NULL;
	atomic_set(&adapter->state, ESP_CONTEXT_DISABLED);
fail:
	esp_usb_cleanup_context(ctx);
	return ret;
}

static void esp_usb_disconnect(struct usb_interface *intf)
{
	struct esp_usb_context *ctx = usb_get_intfdata(intf);
	struct esp_adapter *adapter;

	usb_set_intfdata(intf, NULL);
	if (!ctx)
		return;

	adapter = ctx->adapter;
	if (adapter) {
		set_bit(ESP_TRANSPORT_REMOVING, &adapter->state_flags);
		set_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags);
		clear_bit(ESP_INIT_DONE, &adapter->state_flags);
		atomic_set(&adapter->state, ESP_CONTEXT_DISABLED);
		cancel_delayed_work_sync(&adapter->fw_recovery_work);
		if (adapter->events_wq)
			cancel_work_sync(&adapter->events_work);
		skb_queue_purge(&adapter->events_skb_q);
		if (adapter->if_rx_workqueue)
			cancel_work_sync(&adapter->if_rx_work);
		esp_remove_card(adapter, false);
		adapter->if_context = NULL;
		adapter->dev = NULL;
	}

	esp_usb_cleanup_context(ctx);
	dev_info(&intf->dev, "ESP32-S31 USB transport disconnected\n");
}

static const struct usb_device_id esp_usb_table[] = {
	{ USB_DEVICE(ESP_USB_VENDOR_ID, ESP_USB_PRODUCT_ID_S31) },
	{ }
};
MODULE_DEVICE_TABLE(usb, esp_usb_table);

static struct usb_driver esp_usb_driver = {
	.name = "esp_usb",
	.probe = esp_usb_probe,
	.disconnect = esp_usb_disconnect,
	.id_table = esp_usb_table,
};

int generate_slave_intr(void *context, u8 data)
{
	struct esp_usb_context *ctx = context;

	if (!ctx)
		return -ENODEV;

	if (data & BIT(ESP_CLOSE_DATA_PATH))
		return esp_usb_vendor_request(ctx, ESP_USB_VENDOR_REQ_SOFT_RESET);
	if (data & BIT(ESP_OPEN_DATA_PATH))
		return esp_usb_vendor_request(ctx, ESP_USB_VENDOR_REQ_READY_REPLAY);

	return 0;
}

int esp_init_interface_layer(struct esp_adapter *adapter, u32 speed)
{
	int ret;

	(void)speed;
	if (!adapter)
		return -EINVAL;

	s_adapter = adapter;
	adapter->if_type = ESP_IF_TYPE_USB;
	adapter->if_ops = &esp_usb_if_ops;

	ret = usb_register(&esp_usb_driver);
	if (ret) {
		adapter->if_ops = NULL;
		s_adapter = NULL;
	}
	return ret;
}

void esp_deinit_interface_layer(void)
{
	usb_deregister(&esp_usb_driver);
	s_adapter = NULL;
}

int esp_validate_chipset(struct esp_adapter *adapter, u8 chipset)
{
	if (!adapter)
		return -EINVAL;

	if (chipset != ESP_FIRMWARE_CHIP_ESP32S31) {
		esp_err("Chipset=%s ID=%02x not supported for USB\n",
			esp_chipname_from_id(chipset), chipset);
		adapter->chipset = ESP_FIRMWARE_CHIP_UNRECOGNIZED;
		return -ENODEV;
	}

	adapter->chipset = chipset;
	esp_info("Chipset=%s ID=%02x detected over USB\n",
		 esp_chipname_from_id(chipset), chipset);
	return 0;
}

int esp_adjust_spi_clock(struct esp_adapter *adapter, u8 spi_clk_mhz)
{
	(void)adapter;
	(void)spi_clk_mhz;
	return 0;
}
