// SPDX-License-Identifier: GPL-2.0-only
#ifndef ESP_TRANSPORT_BUF_H
#define ESP_TRANSPORT_BUF_H

#include <linux/dma-mapping.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include "esp.h"

/* Ordinary kernel memory: the bus core/controller owns DMA mapping.
 * Reserve protocol headroom and space to align the mapped payload. */
static inline struct sk_buff *esp_transport_alloc_skb(u32 len, u32 min_len,
						     unsigned int alignment)
{
	struct sk_buff *skb;
	u32 capacity = max(len, min_len);
	unsigned int overhead = INTERFACE_HEADER_PADDING + 2 * alignment;

	if (capacity > U32_MAX - overhead)
		return NULL;
	skb = netdev_alloc_skb(NULL, capacity + overhead);
	if (!skb)
		return NULL;
	skb_reserve(skb, INTERFACE_HEADER_PADDING);
	skb_reserve(skb, PTR_ALIGN(skb->data, alignment) - skb->data);
	return skb;
}

static inline unsigned int esp_transport_dma_alignment(void)
{
	return max_t(unsigned int, SKB_DATA_ADDR_ALIGNMENT,
		     dma_get_cache_alignment());
}

/* Retain the original allocation for kfree(). */
static inline void *esp_transport_alloc_aligned(size_t len, unsigned int align,
					       void **allocation)
{
	*allocation = NULL;
	if (!align || len > SIZE_MAX - align + 1)
		return NULL;
	*allocation = kmalloc(len + align - 1, GFP_KERNEL);
	return *allocation ? PTR_ALIGN(*allocation, align) : NULL;
}

#endif
