// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 */
#ifndef _esp_kernel_port__h_
#define _esp_kernel_port__h_

#include "esp.h"
#include <net/cfg80211.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/timer.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0))
    #define ESP_BT_SEND_FRAME_PROTOTYPE() \
	int esp_bt_send_frame(struct sk_buff *skb)
#else
    #define ESP_BT_SEND_FRAME_PROTOTYPE() \
	int esp_bt_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
static inline void ether_addr_copy(u8 *dst, const u8 *src)
{
	u16 *a = (u16 *)dst;
	const u16 *b = (const u16 *)src;

	a[0] = b[0];
	a[1] = b[1];
	a[2] = b[2];
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
  #define ALLOC_NETDEV(size, name, type, setup) \
    alloc_netdev(size, name, setup)
#else
  #define ALLOC_NETDEV(size, name, type, setup) \
    alloc_netdev(size, name, type, setup)
#endif


#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 18, 0)
  #define CFG80211_INFORM_BSS(wiphy, chan, type, bssid, tsf, \
	  cap, beacon_interval, ie, ielen, sig, gfp) \
  cfg80211_inform_bss(wiphy, chan, bssid, tsf, \
	  cap, beacon_interval, ie, ielen, sig, gfp)
#else
  #define CFG80211_INFORM_BSS(wiphy, chan, type, bssid, tsf, \
	  cap, beacon_interval, ie, ielen, signal, gfp) \
  cfg80211_inform_bss(wiphy, chan, type, bssid, tsf, \
	  cap, beacon_interval, ie, ielen, signal, gfp)
#endif


#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 18, 0)
enum ieee80211_bss_type {
	IEEE80211_BSS_TYPE_ESS,
	IEEE80211_BSS_TYPE_PBSS,
	IEEE80211_BSS_TYPE_IBSS,
	IEEE80211_BSS_TYPE_MBSS,
	IEEE80211_BSS_TYPE_ANY
};

enum ieee80211_privacy {
	IEEE80211_PRIVACY_ON,
	IEEE80211_PRIVACY_OFF,
	IEEE80211_PRIVACY_ANY
};
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
  #define CFG80211_DISCONNECTED(dev, reason, ie, ie_len, locally_generated, gfp) \
    cfg80211_disconnected(dev, reason, ie, ie_len, gfp)

#else
  #define CFG80211_DISCONNECTED(dev, reason, ie, ie_len, locally_generated, gfp) \
      cfg80211_disconnected(dev, reason, ie, ie_len, locally_generated, gfp)
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0))
    #if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34))
        #define hci_skb_pkt_type(skb) bt_cb((skb))->pkt_type
    #else
	#error "ESP-Hosted solution doesn't supported below kernel version < 2.6.34"
    #endif
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 8, 0))
    #if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34))
        #define HCI_PRIMARY HCI_BREDR
    #else
	#error "ESP-Hosted solution doesn't supported below kernel version < 2.6.34"
    #endif
#endif


#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 8, 0)

    #define ESP_MARK_SCAN_DONE(PrIv, abort) do {                               \
									       \
	if (PrIv->request) {                                                   \
	    cfg80211_scan_done(PrIv->request, abort);                          \
	    PrIv->request = NULL;                                              \
	}                                                                      \
									       \
	PrIv->scan_in_progress = false;                                        \
									       \
    } while (0);

#else

    #define ESP_MARK_SCAN_DONE(PrIv, abort) do {                               \
									       \
	struct cfg80211_scan_info info = {                                     \
	    .aborted = abort,                                                  \
	};                                                                     \
									       \
	if (PrIv->request) {                                                   \
	    cfg80211_scan_done(PrIv->request, &info);                          \
	    PrIv->request = NULL;                                              \
	}                                                                      \
									       \
	PrIv->scan_in_progress = false;                                        \
									       \
    } while (0);

#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0)
static inline void *skb_put_data(struct sk_buff *skb, const void *data,
				 unsigned int len)
{
	void *tmp = skb_put(skb, len);

	memcpy(tmp, data, len);

	return tmp;
}
#endif

/* kernel_read() argument order changed in 4.14: offset moved last and became in/out. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
static inline ssize_t esp_kernel_read(struct file *file, void *buf,
				      size_t count, loff_t *pos)
{
	ssize_t nread;

	nread = kernel_read(file, *pos, buf, count);
	if (nread > 0)
		*pos += nread;

	return nread;
}
#else
#define esp_kernel_read kernel_read
#endif

/*
 * timer_setup() replaced setup_timer() in 4.15. Some 4.9/4.14 LTS kernels
 * backported it with from_timer(); 6.16+ dropped from_timer and made
 * timer_setup() a macro. Only provide a shim when none of that exists.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0) && \
	!defined(from_timer) && !defined(timer_setup)
#define from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*var), timer_fieldname)

static inline void timer_setup(struct timer_list *timer,
			       void (*callback)(struct timer_list *),
			       unsigned int flags)
{
	setup_timer(timer, (void (*)(unsigned long))callback,
		    (unsigned long)timer);
	(void)flags;
}
#endif

/* cfg80211 renamed sae_data to auth_data in 4.12. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
#define ESP_CFG80211_AUTH_DATA(req)		((req)->auth_data)
#define ESP_CFG80211_AUTH_DATA_LEN(req)		((req)->auth_data_len)
#else
#define ESP_CFG80211_AUTH_DATA(req)		((req)->sae_data)
#define ESP_CFG80211_AUTH_DATA_LEN(req)		((req)->sae_data_len)
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0))
#define do_exit(code)	kthread_complete_and_exit(NULL, code)
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0))
#define NETIF_RX_NI(skb)	netif_rx(skb)
#else
#define NETIF_RX_NI(skb)	netif_rx_ni(skb)
#endif

static inline
void CFG80211_RX_ASSOC_RESP(struct net_device *dev,
			    struct cfg80211_bss *bss,
			    const u8 *buf, size_t len,
			    int uapsd_queues,
			    const u8 *req_ies, size_t req_ies_len)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0))

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0))
	struct cfg80211_rx_assoc_resp_data resp = {0};
#else
	struct cfg80211_rx_assoc_resp resp = {0};
#endif

	resp.links[0].bss = bss;
	resp.buf = (u8 *)buf;
	resp.len =  len;
	resp.req_ies = req_ies;
	resp.uapsd_queues = uapsd_queues;
	resp.req_ies_len = req_ies_len;

	cfg80211_rx_assoc_resp(dev, &resp);
#else
	#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 21))
	cfg80211_rx_assoc_resp(dev, bss, buf, len, uapsd_queues, req_ies, req_ies_len);
	#else
	cfg80211_rx_assoc_resp(dev, bss, buf, len, uapsd_queues);
	#endif
#endif
}


#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0))
static inline bool wireless_dev_current_bss_exists(struct wireless_dev *wdev)
{
	if (wdev->links[0].client.current_bss)
		return true;
	return false;
}
#define INT_LINK_ID int link_id,
#define ZERO_LINK_ID 0,
#else
static inline bool wireless_dev_current_bss_exists(struct wireless_dev *wdev)
{
	if (wdev->current_bss)
		return true;
	return false;
}
#define INT_LINK_ID
#define ZERO_LINK_ID
#endif

/* In kernel version 5.15 and later, eth_hw_addr_set() is provided by the kernel.
 * However, in LTS kernels, this function has been backported in later releases.
 * To maintain compatibility with older kernels (pre-5.15), we define a macro
 * ETH_HW_ADDR_SET that either uses ether_addr_copy function equivalent (ether_addr_copy)
 * for kernels < 5.15 or the kernel-provided eth_hw_addr_set() for 5.15 and above.
 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0))
/* For kernel versions < 5.15, use ether_addr_copy() to set the hardware address. */
#define ETH_HW_ADDR_SET(dev, addr) ether_addr_copy((dev)->dev_addr, (addr))
#else
/* For kernel versions >= 5.15, use the kernel-provided eth_hw_addr_set(). */
#define ETH_HW_ADDR_SET(dev, addr) eth_hw_addr_set(dev, addr)
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0))
#define spi_master			spi_controller
#define spi_master_put(_ctlr)		spi_controller_put(_ctlr)
#endif


#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
  #define del_timer timer_delete_sync
#endif

#endif
