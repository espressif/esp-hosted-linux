// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "utils.h"
#include "esp.h"
#include "esp_api.h"
#include "esp_cfg80211.h"
#include "esp_cmd.h"
#include "esp_kernel_port.h"
#include "esp_utils.h"

/* WiFi PHY rate encodings. */
typedef enum {
	WIFI_PHY_RATE_1M_L      = 0x00, /**< 1 Mbps with long preamble */
	WIFI_PHY_RATE_2M_L      = 0x01, /**< 2 Mbps with long preamble */
	WIFI_PHY_RATE_5M_L      = 0x02, /**< 5.5 Mbps with long preamble */
	WIFI_PHY_RATE_11M_L     = 0x03, /**< 11 Mbps with long preamble */
	WIFI_PHY_RATE_2M_S      = 0x05, /**< 2 Mbps with short preamble */
	WIFI_PHY_RATE_5M_S      = 0x06, /**< 5.5 Mbps with short preamble */
	WIFI_PHY_RATE_11M_S     = 0x07, /**< 11 Mbps with short preamble */
	WIFI_PHY_RATE_48M       = 0x08, /**< 48 Mbps */
	WIFI_PHY_RATE_24M       = 0x09, /**< 24 Mbps */
	WIFI_PHY_RATE_12M       = 0x0A, /**< 12 Mbps */
	WIFI_PHY_RATE_6M        = 0x0B, /**< 6 Mbps */
	WIFI_PHY_RATE_54M       = 0x0C, /**< 54 Mbps */
	WIFI_PHY_RATE_36M       = 0x0D, /**< 36 Mbps */
	WIFI_PHY_RATE_18M       = 0x0E, /**< 18 Mbps */
	WIFI_PHY_RATE_9M        = 0x0F, /**< 9 Mbps */
	WIFI_PHY_RATE_MCS0_LGI  = 0x10, /**< MCS0 with long GI, 6.5 Mbps for 20MHz, 13.5 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS1_LGI  = 0x11, /**< MCS1 with long GI, 13 Mbps for 20MHz, 27 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS2_LGI  = 0x12, /**< MCS2 with long GI, 19.5 Mbps for 20MHz, 40.5 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS3_LGI  = 0x13, /**< MCS3 with long GI, 26 Mbps for 20MHz, 54 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS4_LGI  = 0x14, /**< MCS4 with long GI, 39 Mbps for 20MHz, 81 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS5_LGI  = 0x15, /**< MCS5 with long GI, 52 Mbps for 20MHz, 108 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS6_LGI  = 0x16, /**< MCS6 with long GI, 58.5 Mbps for 20MHz, 121.5 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS7_LGI  = 0x17, /**< MCS7 with long GI, 65 Mbps for 20MHz, 135 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS0_SGI  = 0x18, /**< MCS0 with short GI, 7.2 Mbps for 20MHz, 15 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS1_SGI  = 0x19, /**< MCS1 with short GI, 14.4 Mbps for 20MHz, 30 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS2_SGI  = 0x1A, /**< MCS2 with short GI, 21.7 Mbps for 20MHz, 45 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS3_SGI  = 0x1B, /**< MCS3 with short GI, 28.9 Mbps for 20MHz, 60 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS4_SGI  = 0x1C, /**< MCS4 with short GI, 43.3 Mbps for 20MHz, 90 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS5_SGI  = 0x1D, /**< MCS5 with short GI, 57.8 Mbps for 20MHz, 120 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS6_SGI  = 0x1E, /**< MCS6 with short GI, 65 Mbps for 20MHz, 135 Mbps for 40MHz */
	WIFI_PHY_RATE_MCS7_SGI  = 0x1F, /**< MCS7 with short GI, 72.2 Mbps for 20MHz, 150 Mbps for 40MHz */
	WIFI_PHY_RATE_LORA_250K = 0x29, /**< 250 Kbps */
	WIFI_PHY_RATE_LORA_500K = 0x2A, /**< 500 Kbps */
	WIFI_PHY_RATE_MAX,
} wifi_phy_rate_t;

/* Supported rates to be advertised to the cfg80211 */
static struct ieee80211_rate esp_rates[] = {
	{.bitrate = 10, .hw_value = WIFI_PHY_RATE_1M_L, },
	{.bitrate = 20, .hw_value = WIFI_PHY_RATE_2M_L, },
	{.bitrate = 55, .hw_value = WIFI_PHY_RATE_5M_L, .hw_value_short = WIFI_PHY_RATE_5M_S},
	{.bitrate = 110, .hw_value = WIFI_PHY_RATE_11M_L, .hw_value_short = WIFI_PHY_RATE_11M_S},
	{.bitrate = 60, .hw_value = WIFI_PHY_RATE_6M, },
	{.bitrate = 90, .hw_value = WIFI_PHY_RATE_9M, },
	{.bitrate = 120, .hw_value = WIFI_PHY_RATE_12M, },
	{.bitrate = 180, .hw_value = WIFI_PHY_RATE_18M, },
	{.bitrate = 240, .hw_value = WIFI_PHY_RATE_24M, },
	{.bitrate = 360, .hw_value = WIFI_PHY_RATE_36M, },
	{.bitrate = 480, .hw_value = WIFI_PHY_RATE_48M, },
	{.bitrate = 540, .hw_value = WIFI_PHY_RATE_54M, },
};


/* Channel definitions to be advertised to cfg80211 */
static struct ieee80211_channel esp_channels_2ghz[] = {
	{.center_freq = 2412, .hw_value = 1, .max_power = 20, },
	{.center_freq = 2417, .hw_value = 2, .max_power = 20, },
	{.center_freq = 2422, .hw_value = 3, .max_power = 20, },
	{.center_freq = 2427, .hw_value = 4, .max_power = 20, },
	{.center_freq = 2432, .hw_value = 5, .max_power = 20, },
	{.center_freq = 2437, .hw_value = 6, .max_power = 20, },
	{.center_freq = 2442, .hw_value = 7, .max_power = 20, },
	{.center_freq = 2447, .hw_value = 8, .max_power = 20, },
	{.center_freq = 2452, .hw_value = 9, .max_power = 20, },
	{.center_freq = 2457, .hw_value = 10, .max_power = 20, },
	{.center_freq = 2462, .hw_value = 11, .max_power = 20, },
	{.center_freq = 2467, .hw_value = 12, .max_power = 20, },
	{.center_freq = 2472, .hw_value = 13, .max_power = 20, },
	{.center_freq = 2484, .hw_value = 14, .max_power = 20, },
};

static struct ieee80211_channel esp_channels_5ghz[] = {
	{.center_freq = 5180, .hw_value = 36, .max_power = 20, },
	{.center_freq = 5200, .hw_value = 40, .max_power = 20, },
	{.center_freq = 5220, .hw_value = 44, .max_power = 20, },
	{.center_freq = 5240, .hw_value = 48, .max_power = 20, },
	{.center_freq = 5260, .hw_value = 52, .max_power = 20, },
	{.center_freq = 5280, .hw_value = 56, .max_power = 20, },
	{.center_freq = 5300, .hw_value = 60, .max_power = 20, },
	{.center_freq = 5320, .hw_value = 64, .max_power = 20, },
	{.center_freq = 5500, .hw_value = 100, .max_power = 20, },
	{.center_freq = 5520, .hw_value = 104, .max_power = 20, },
	{.center_freq = 5540, .hw_value = 108, .max_power = 20, },
	{.center_freq = 5560, .hw_value = 112, .max_power = 20, },
	{.center_freq = 5580, .hw_value = 116, .max_power = 20, },
	{.center_freq = 5600, .hw_value = 120, .max_power = 20, },
	{.center_freq = 5620, .hw_value = 124, .max_power = 20, },
	{.center_freq = 5640, .hw_value = 128, .max_power = 20, },
	{.center_freq = 5660, .hw_value = 132, .max_power = 20, },
	{.center_freq = 5680, .hw_value = 136, .max_power = 20, },
	{.center_freq = 5700, .hw_value = 140, .max_power = 20, },
	{.center_freq = 5720, .hw_value = 144, .max_power = 20, },
	{.center_freq = 5745, .hw_value = 149, .max_power = 20, },
	{.center_freq = 5765, .hw_value = 153, .max_power = 20, },
	{.center_freq = 5785, .hw_value = 157, .max_power = 20, },
	{.center_freq = 5805, .hw_value = 161, .max_power = 20, },
	{.center_freq = 5825, .hw_value = 165, .max_power = 20, },
	{.center_freq = 5845, .hw_value = 169, .max_power = 20, },
	{.center_freq = 5865, .hw_value = 173, .max_power = 20, },
	{.center_freq = 5885, .hw_value = 177, .max_power = 20, },
};

static struct ieee80211_supported_band esp_wifi_bands_2ghz = {
	.channels = esp_channels_2ghz,
	.n_channels = ARRAY_SIZE(esp_channels_2ghz),
	.bitrates = esp_rates,
	.n_bitrates = ARRAY_SIZE(esp_rates),
	.ht_cap.cap = IEEE80211_HT_CAP_SUP_WIDTH_20_40 | IEEE80211_HT_CAP_SGI_20 |
			IEEE80211_HT_CAP_RX_STBC | IEEE80211_HT_CAP_DSSSCCK40,
	.ht_cap.mcs.rx_mask[0] = 0xff,
	.ht_cap.mcs.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
	.ht_cap.ht_supported = true,
};

static struct ieee80211_supported_band esp_wifi_bands_5ghz = {
	.channels = esp_channels_5ghz,
	.n_channels = ARRAY_SIZE(esp_channels_5ghz),
	.bitrates = esp_rates + 4,
	.n_bitrates = ARRAY_SIZE(esp_rates) - 4,
	.ht_cap.cap = IEEE80211_HT_CAP_SUP_WIDTH_20_40 | IEEE80211_HT_CAP_SGI_20 |
			IEEE80211_HT_CAP_RX_STBC | IEEE80211_HT_CAP_DSSSCCK40,
	.ht_cap.mcs.rx_mask[0] = 0xff,
	.ht_cap.mcs.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
	.ht_cap.ht_supported = true,
	.vht_cap.vht_supported = true,
	.vht_cap.cap = (IEEE80211_VHT_MAX_AMPDU_16K << IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT) |
			IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_3895 | IEEE80211_VHT_CAP_RXSTBC_1,
	.vht_cap.vht_mcs.rx_mcs_map = cpu_to_le16(IEEE80211_VHT_MCS_SUPPORT_0_7 << 0  |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 2  |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 4  |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 6  |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 8  |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 10 |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 12 |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 14),
	.vht_cap.vht_mcs.rx_highest = 0x0,
	.vht_cap.vht_mcs.tx_mcs_map = cpu_to_le16(IEEE80211_VHT_MCS_SUPPORT_0_7 << 0  |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 2  |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 4  |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 6  |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 8  |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 10 |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 12 |
			IEEE80211_VHT_MCS_NOT_SUPPORTED << 14),
	.vht_cap.vht_mcs.tx_highest = 0x0,
};

/* Supported crypto cipher suits to be advertised to cfg80211 */
static const u32 esp_cipher_suites[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_SMS4,
	WLAN_CIPHER_SUITE_AES_CMAC,
};

static const u32 esp_cipher_suites_new[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_SMS4,
	WLAN_CIPHER_SUITE_GCMP,
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_GCMP_256,
	WLAN_CIPHER_SUITE_AES_CMAC,
	WLAN_CIPHER_SUITE_BIP_GMAC_128,
	WLAN_CIPHER_SUITE_BIP_GMAC_256,
};

static const struct wiphy_wowlan_support esp_wowlan_support = {
	.flags = WIPHY_WOWLAN_ANY | WIPHY_WOWLAN_MAGIC_PKT | WIPHY_WOWLAN_DISCONNECT | WIPHY_WOWLAN_4WAY_HANDSHAKE,
	.n_patterns = 0,
	.pattern_max_len = 0,
	.pattern_min_len = 0,
	.max_pkt_offset = 0,
};

/* TODO get MAX_TX_POWER_MBM from Firmware for future chips */
#define MAX_TX_POWER_MBM (20 * 100)
#define MIN_TX_POWER_MBM (8 * 100)
static bool is_txpwr_valid(int mbm)
{
	if (mbm > MAX_TX_POWER_MBM)
		return false;

	if (mbm < MIN_TX_POWER_MBM)
		return false;

	return true;
}

static int mbm_to_esp_pwr(int mbm)
{
	return ((mbm * 4) / 100);
}

static int esp_pwr_to_dbm(int power)
{
	return ((power / 4));
}

static int esp_inetaddr_event(struct notifier_block *nb,
	unsigned long event, void *data)
{
	struct in_ifaddr *ifa = data;
	struct net_device *netdev = ifa->ifa_dev ? ifa->ifa_dev->dev : NULL;
	struct esp_wifi_device *priv;
	struct esp_adapter *adapter = esp_get_adapter();
	struct esp_wifi_device *esp_priv = adapter->priv[0];

	if (!netdev)
		return 0;

	esp_verbose("------- IP event for interface %s -------\n", netdev->name);

	priv = netdev_priv(netdev);
	if (esp_priv != priv)
		return 0;

	switch (event) {

	case NETDEV_UP:
		esp_info("NETDEV_UP interface %s ip changed to  %pi4\n",
				netdev->name, &ifa->ifa_local);
		if (priv && (priv->if_type == ESP_STA_IF))
			cmd_set_ip_address(priv, ifa->ifa_local);
		break;

	case NETDEV_DOWN:
		if (priv && (priv->if_type == ESP_STA_IF)) {
			cmd_set_ip_address(priv, cpu_to_be32(0));
			esp_info("Interface %s Down: %d\n", netdev->name, priv->if_type);
		}
		break;
	}

	return 0;
}

struct wireless_dev *esp_cfg80211_add_iface(struct wiphy *wiphy,
		const char *name,
		unsigned char name_assign_type,
		enum nl80211_iftype type,
		struct vif_params *params)
{
	struct esp_device *esp_dev = NULL;
/*	struct wireless_dev *wdev = NULL;*/
	struct net_device *ndev;
	struct esp_wifi_device *esp_wdev;
	uint8_t esp_nw_if_num = 0;
	bool fw_initialized = false;

	if (!wiphy || !name) {
		esp_info("%u invalid input\n", __LINE__);
		return NULL;
	}

	esp_dev = wiphy_priv(wiphy);

	if (!esp_dev || !esp_dev->adapter) {
		esp_info("%u invalid input\n", __LINE__);
		return NULL;
	}

	if (type == NL80211_IFTYPE_STATION) {
		esp_nw_if_num = ESP_STA_NW_IF;
	} else if (type == NL80211_IFTYPE_AP) {
		esp_nw_if_num = ESP_AP_NW_IF;
	} else {
		esp_info("%u network type[%u] is not supported\n",
				 __LINE__, type);
		return NULL;
	}

	ndev = ALLOC_NETDEV(sizeof(struct esp_wifi_device), name, name_assign_type,
			ether_setup);

	if (!ndev)
		return ERR_PTR(-ENOMEM);

	set_bit(ESP_DRIVER_ACTIVE, &esp_dev->adapter->state_flags);
	esp_wdev = netdev_priv(ndev);

	ndev->ieee80211_ptr = &esp_wdev->wdev;
	esp_wdev->wdev.wiphy = wiphy;
	esp_wdev->esp_dev = esp_dev;
	esp_wdev->ndev = ndev;
	esp_wdev->adapter = esp_dev->adapter;
	esp_wdev->adapter->priv[esp_nw_if_num] = esp_wdev;
	esp_wdev->tx_pwr = mbm_to_esp_pwr(MAX_TX_POWER_MBM);
	esp_verbose("Updated priv[%u] to %px\n",
                esp_nw_if_num, esp_wdev->adapter->priv[esp_nw_if_num]);
	dev_net_set(ndev, wiphy_net(wiphy));
	SET_NETDEV_DEV(ndev, wiphy_dev(esp_wdev->wdev.wiphy));
	esp_wdev->wdev.netdev = ndev;
	esp_wdev->wdev.iftype = type;

	init_waitqueue_head(&esp_wdev->wait_for_scan_completion);
	spin_lock_init(&esp_wdev->bss_lock);
	esp_mlme_init(esp_wdev);
	esp_wdev->stop_data = 1;
	esp_wdev->port_open = 0;

#ifdef TODO
	if (cmd_update_fw_time(esp_wdev))
		goto free_and_return;
#endif

	if (cmd_init_interface(esp_wdev))
		goto free_and_return;
	fw_initialized = true;
	set_bit(ESP_INTERFACE_INITIALIZED, &esp_wdev->priv_flags);

	if (cmd_get_mac(esp_wdev))
		goto free_and_return;

	ETH_HW_ADDR_SET(ndev, esp_wdev->mac_address);

	esp_init_priv(ndev);

	/* Do not publish wlan0 while reconstruction still rejects ordinary
	 * cfg80211 commands. Boot/reload registers after INIT_DONE. */
	if (!test_bit(ESP_ALLOW_RECONSTRUCT, &esp_dev->adapter->state_flags)) {
		if (esp_cfg80211_register_iface(esp_wdev))
			goto free_and_return;
	}

	return &esp_wdev->wdev;

free_and_return:
	if (fw_initialized)
		cmd_deinit_interface(esp_wdev);
	if (esp_wdev->adapter->priv[esp_nw_if_num] == esp_wdev)
		esp_wdev->adapter->priv[esp_nw_if_num] = NULL;
	clear_bit(ESP_DRIVER_ACTIVE, &esp_wdev->adapter->state_flags);
	esp_mlme_cancel(esp_wdev);
	dev_net_set(ndev, NULL);
	free_netdev(ndev);
	return NULL;
}

int esp_cfg80211_register_iface(struct esp_wifi_device *priv)
{
	struct net_device *ndev;

	if (!priv || !priv->ndev)
		return -EINVAL;
	if (test_bit(ESP_NETWORK_UP, &priv->priv_flags))
		return 0;

	ndev = priv->ndev;
	if (register_netdevice(ndev))
		return -EIO;

	set_bit(ESP_NETWORK_UP, &priv->priv_flags);
	priv->nb.notifier_call = esp_inetaddr_event;
	register_inetaddr_notifier(&priv->nb);
	return 0;
}

#if 0
static int esp_cfg80211_del_iface(struct wiphy *wiphy,
							  struct wireless_dev *wdev)
{
	return 0;
}
#endif

static int esp_nl_mode_to_esp_iface(enum nl80211_iftype type)
{
	if (type == NL80211_IFTYPE_STATION) {
		return ESP_STA_IF;
	} else if (type == NL80211_IFTYPE_AP) {
		return ESP_AP_IF;
	}

	return ESP_MAX_IF;
}

static int8_t esp_get_mode_from_iface_type(int iface_type)
{
	if (iface_type == ESP_AP_IF) {
		return WIFI_MODE_APSTA;
	}

	return WIFI_MODE_STA;
}

static int esp_cfg80211_change_iface(struct wiphy *wiphy,
					struct net_device *dev,
					enum nl80211_iftype type,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
					u32 *flags __maybe_unused,
#endif
					struct vif_params *params)
{
	struct esp_wifi_device *priv = NULL;
	enum ESP_INTERFACE_TYPE esp_if_type;
	int ret;

	if (!wiphy || !dev) {
		esp_err("%u invalid params\n", __LINE__);
		return -EINVAL;
	}

	priv = netdev_priv(dev);
	if (!priv) {
		esp_err("empty priv\n");
		return -EINVAL;
	}

	esp_if_type = esp_nl_mode_to_esp_iface(type);
	esp_info("current iface type=%d new iface type=%d\n", priv->if_type, esp_if_type);
	if (esp_if_type == ESP_MAX_IF) {
		esp_err("%u invalid params\n", __LINE__);
		return -EINVAL;
	}

	if (esp_if_type == priv->if_type) {
		esp_info("%u operating in same mode\n", __LINE__);
		return 0;
	}

	ret = cmd_set_mode(priv, esp_get_mode_from_iface_type(esp_if_type));

	if (ret == 0) {
		priv->if_type = esp_if_type;
		priv->wdev.iftype = type;
		/* update Mac address of interface */
		cmd_get_mac(priv);
		ETH_HW_ADDR_SET(dev, priv->mac_address/*mac_addr->sa_data*/);
	}

	esp_info("wdev iftype=%d, ret=%d\n", priv->wdev.iftype, ret);

	return ret;
}

static int esp_cfg80211_scan(struct wiphy *wiphy,
		struct cfg80211_scan_request *request)
{

	struct net_device *ndev = NULL;
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !request || !request->wdev || !request->wdev->netdev) {
		esp_info("%u invalid input\n", __LINE__);
		return -EINVAL;
	}

	ndev = request->wdev->netdev;
	priv = netdev_priv(ndev);

	esp_dbg("\n");
	if (!priv) {
		esp_err("Empty priv\n");
		return -EINVAL;
	}

	return cmd_scan_request(priv, request);
}

#if 0
static int esp_cfg80211_connect(struct wiphy *wiphy, struct net_device *dev,
							  struct cfg80211_connect_params *sme)
{
	struct esp_wifi_device *priv = netdev_priv(dev);

	esp_dbg("\n");
	if (!priv) {
		esp_err("Empty priv\n");
		return -EINVAL;
	}

	return cmd_connect_request(priv, sme);
}
#endif

static int esp_cfg80211_mgmt_tx(struct wiphy *wiphy, struct wireless_dev *wdev,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0))
				struct ieee80211_channel *chan,
				bool offchan, unsigned int wait, const u8 *buf, size_t len,
				bool no_cck, bool dont_wait_for_ack,
#else
				struct cfg80211_mgmt_tx_params *params,
#endif
		u64 *cookie)
{
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !wdev || !params) {
		esp_info("%u invalid input\n", __LINE__);
		return -EINVAL;
	}

	priv = netdev_priv(wdev->netdev);

	if (!priv) {
		esp_err("empty priv\n");
		return -EINVAL;
	}

	return cmd_mgmt_request(priv, params, cookie);
}

static int esp_cfg80211_set_ap_chanwidth(struct wiphy *wiphy,
					 struct net_device *dev,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 2))
					 unsigned int link_id,
#endif
					 struct cfg80211_chan_def *chandef)
{
	esp_info("%u \n", __LINE__);
	return -EOPNOTSUPP;
}

static int esp_cfg80211_set_default_key(struct wiphy *wiphy,
					struct net_device *dev, INT_LINK_ID
					u8 key_index, bool unicast, bool multicast)
{
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !dev) {
		esp_err("%u invalid params\n", __LINE__);
		return -EINVAL;
	}

	priv = netdev_priv(dev);
	if (!priv) {
		esp_err("Empty priv");
		return -EINVAL;
	}

	return cmd_set_default_key(priv, key_index);
}

static int esp_cfg80211_set_default_mgmt_key(struct wiphy *wiphy,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))
					     struct wireless_dev *wdev, INT_LINK_ID u8 key_index)
#else
					     struct net_device *ndev, INT_LINK_ID u8 key_index)
#endif
{
	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))
static int esp_cfg80211_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
#else
static int esp_cfg80211_del_key(struct wiphy *wiphy, struct net_device *dev,
#endif
				INT_LINK_ID u8 key_index, bool pairwise,
				const u8 *mac_addr)
{
	struct esp_wifi_device *priv = NULL;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))
	if (!wdev || !wdev->netdev)
		return -EINVAL;
	priv = netdev_priv(wdev->netdev);
#else
	if (!dev)
		return -EINVAL;
	priv = netdev_priv(dev);
#endif
	if (!priv)
		return -EINVAL;

	return cmd_del_key(priv, key_index, pairwise, mac_addr);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))
static int esp_cfg80211_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
				INT_LINK_ID u8 key_index, bool pairwise,
				const u8 *mac_addr, struct key_params *params)
{
	struct net_device *dev = wdev->netdev;
#else
static int esp_cfg80211_add_key(struct wiphy *wiphy, struct net_device *dev,
				INT_LINK_ID u8 key_index, bool pairwise,
				const u8 *mac_addr, struct key_params *params)
{
#endif
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !dev || !params) {
		esp_err("%u invalid params\n", __LINE__);
		return -EINVAL;
	}

	priv = netdev_priv(dev);
	if (!priv) {
		esp_err("Empty priv\n");
		return -EINVAL;
	}
	esp_dbg("\n");

	if (params->key_len == 0) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))
		return esp_cfg80211_del_key(wiphy, wdev, ZERO_LINK_ID key_index, pairwise, mac_addr);
#else
		return esp_cfg80211_del_key(wiphy, dev, ZERO_LINK_ID key_index, pairwise, mac_addr);
#endif
	}
	return cmd_add_key(priv, key_index, pairwise, mac_addr, params);
}

static int esp_cfg80211_disconnect(struct wiphy *wiphy,
		struct net_device *dev, u16 reason_code)
{
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !dev) {
		esp_info("%u invalid input\n",  __LINE__);
		return -EINVAL;
	}

	priv = netdev_priv(dev);

	if (!priv) {
		esp_err("empty priv\n");
		return 0;
	}
	esp_dbg("\n");

	return cmd_disconnect_request(priv, reason_code, NULL, DISCONNECT_TYPE_DEAUTH);
}

static void esp_retire_stale_disconnect_generation(struct esp_wifi_device *priv)
{
	struct sk_buff *staged_assoc = NULL;
	struct sk_buff *staged_disc = NULL;

	if (!priv)
		return;

	/* A successful local disconnect may be followed by a lost async event.
	 * Once cfg80211 starts a new AUTH/ASSOC generation, that old local marker
	 * must no longer be able to classify a later remote disconnect as local.
	 * Do not disturb a disconnect command that is still synchronously pending.
	 * If a staged remote disconnect from the established connection is pending,
	 * deliver it rather than discarding it. */
	spin_lock_bh(&priv->bss_lock);
	if (!priv->disconnect_cmd_pending) {
		staged_disc = priv->staged_disconnect_skb;
		priv->staged_disconnect_skb = NULL;
	}
	if (!priv->assoc_cmd_pending) {
		priv->staged_assoc_seq = 0;
		priv->assoc_awaiting_mlme = false;
		staged_assoc = priv->staged_assoc_skb;
		priv->staged_assoc_skb = NULL;
	}
	spin_unlock_bh(&priv->bss_lock);

	if (staged_disc) {
		esp_deliver_disconnect_from_skb(priv, staged_disc);
		kfree_skb(staged_disc);
	}

	spin_lock_bh(&priv->bss_lock);
	if (!priv->disconnect_cmd_pending) {
		priv->local_disconnect_req = false;
		priv->disconnect_awaiting_mlme = false;
		priv->disconnect_cmd_seq = 0;
		if (!wireless_dev_current_bss_exists(&priv->wdev))
			priv->conn_generation = 0;
		memset(priv->disconnect_bssid, 0, MAC_ADDR_LEN);
	}
	spin_unlock_bh(&priv->bss_lock);

	kfree_skb(staged_assoc);
}

static int esp_cfg80211_authenticate(struct wiphy *wiphy, struct net_device *dev,
		struct cfg80211_auth_request *req)
{
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !dev || !req) {
		esp_info("%u invalid input\n", __LINE__);
		return -EINVAL;
	}

	esp_dbg("\n");

	priv = netdev_priv(dev);

	if (!priv) {
		esp_err("Empty priv\n");
		return 0;
	}

	if (priv->if_type == ESP_STA_IF)
		esp_retire_stale_disconnect_generation(priv);

	return cmd_auth_request(priv, req);
}


static int esp_cfg80211_associate(struct wiphy *wiphy, struct net_device *dev,
		struct cfg80211_assoc_request *req)
{
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !dev || !req) {
		esp_info("%u invalid input\n", __LINE__);
		return -EINVAL;
	}

	priv = netdev_priv(dev);

	esp_dbg("\n");

	if (!priv) {
		esp_err("Empty priv\n");
		return -EINVAL;
	}

	if (priv->if_type == ESP_STA_IF)
		esp_retire_stale_disconnect_generation(priv);

	return cmd_assoc_request(priv, req);
}

static int esp_cfg80211_deauth(struct wiphy *wiphy, struct net_device *dev,
		struct cfg80211_deauth_request *req)
{
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !dev || !req) {
		esp_info("%u invalid input\n", __LINE__);
		return -EINVAL;
	}

	esp_dbg("\n");
	priv = netdev_priv(dev);

	if (!priv) {
		esp_err("Empty priv\n");
		return 0;
	}

	if (req->ie_len > 0) {
		esp_err("Deauth IEs not supported\n");
		return -EOPNOTSUPP;
	}

	if (req->local_state_change) {
		esp_err("Local state change deauth not supported\n");
		return -EOPNOTSUPP;
	}

	return cmd_disconnect_request(priv, req->reason_code, req->bssid, DISCONNECT_TYPE_DEAUTH);
}

static int esp_cfg80211_disassoc(struct wiphy *wiphy, struct net_device *dev,
		struct cfg80211_disassoc_request *req)
{
	struct esp_wifi_device *priv = NULL;
	const u8 *bssid = NULL;

	if (!wiphy || !dev || !req) {
		esp_info("%u invalid input\n", __LINE__);
		return -EINVAL;
	}

	esp_dbg("\n");
	priv = netdev_priv(dev);

	if (!priv) {
		esp_err("Empty priv\n");
		return 0;
	}

	if (req->ie_len > 0) {
		esp_err("Disassoc IEs not supported\n");
		return -EOPNOTSUPP;
	}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0))
	bssid = req->ap_addr;
#else
	if (req->bss)
		bssid = req->bss->bssid;
#endif

	if (req->local_state_change) {
		esp_err("Local state change disassoc not supported\n");
		return -EOPNOTSUPP;
	}

	return cmd_disconnect_request(priv, req->reason_code, bssid, DISCONNECT_TYPE_DISASSOC);
}

static int esp_cfg80211_suspend(struct wiphy *wiphy,
			struct cfg80211_wowlan *wowlan)
{
	struct esp_adapter *adapter = esp_get_adapter();
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !adapter) {
		esp_info("%u invalid input wiphy=%p adapter=%p\n", __LINE__, wiphy, adapter);
		return -EINVAL;
	}

	if (!wowlan) {
		esp_dbg("%u wow config is not set, still suspending\n", __LINE__);
		return 0;
	}

	esp_dbg("wow any=%d disconnect=%d magic_pkt=%d four_way_handshake=%d eap_identity_req=%d",
		    wowlan->any, wowlan->disconnect, wowlan->magic_pkt,
		    wowlan->four_way_handshake, wowlan->eap_identity_req);

	priv = adapter->priv[0];
	if (!priv) {
		esp_err("Empty priv\n");
		return 0;
	}

	return cmd_set_wow_config(priv, wowlan);
}

static int esp_cfg80211_resume(struct wiphy *wiphy)
{
	struct esp_adapter *adapter = esp_get_adapter();
	struct esp_wifi_device *priv = NULL;
	struct cfg80211_wowlan wowlan = {0};

	esp_dbg("\n");
	if (!wiphy || !adapter) {
		esp_info("%u invalid input %p %p\n", __LINE__, wiphy, adapter);
		return -EINVAL;
	}

	priv = adapter->priv[0];
	if (!priv) {
		esp_err("Empty priv\n");
		return 0;
	}

	return cmd_set_wow_config(priv, &wowlan);
}

static void esp_cfg80211_set_wakeup(struct wiphy *wiphy,
			bool enabled)
{
	/*esp_dbg("\n");*/
}

static int esp_cfg80211_set_tx_power(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0))
				     int radio_idx,
#endif
				     enum nl80211_tx_power_setting type, int mbm)
{
	struct esp_adapter *adapter = esp_get_adapter();
	struct esp_wifi_device *priv = NULL;
	enum nl80211_tx_power_setting new_type;
	int new_pwr;
	int ret;

	if (!wiphy || !adapter) {
		esp_info("%u invalid input %p %p \n", __LINE__, wiphy, wdev);
		return -EINVAL;
	}

	esp_dbg("\n");

	priv = adapter->priv[0];
	if (!priv) {
		esp_err("Empty priv\n");
		return 0;
	}

        switch (type) {
        case NL80211_TX_POWER_AUTOMATIC:
                new_type = NL80211_TX_POWER_AUTOMATIC;
                new_pwr = mbm_to_esp_pwr(MAX_TX_POWER_MBM);
                break;
        case NL80211_TX_POWER_LIMITED:
                if (!is_txpwr_valid(mbm)) {
                        esp_warn("mbm:%d not support\n", mbm);
			return -EINVAL;
                }
                new_type = NL80211_TX_POWER_LIMITED;
                new_pwr = mbm_to_esp_pwr(mbm);
                break;
        case NL80211_TX_POWER_FIXED:
		return -EOPNOTSUPP;
                break;
        default:
                esp_warn("unknown type:%d\n", type);
		return -EINVAL;
        }

	ret = cmd_set_tx_power(priv, new_pwr);
	if (ret == 0) {
		priv->tx_pwr_type = new_type;
	}
	return ret;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))
static int esp_cfg80211_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
				    const u8 *mac, struct station_info *sinfo)
{
	struct net_device *ndev = wdev->netdev;
#else
static int esp_cfg80211_get_station(struct wiphy *wiphy, struct net_device *ndev,
				    const u8 *mac, struct station_info *sinfo)
{
#endif
	struct esp_wifi_device *priv = NULL;

	priv = netdev_priv(ndev);

	if (!mac || !priv) {
		esp_err("mac=%p priv=%p\n", mac, priv);
		return -ENOENT;
	}
	if (wireless_dev_current_bss_exists(&priv->wdev)) {

		sinfo->filled |= BIT(NL80211_STA_INFO_SIGNAL);
		cmd_get_rssi(priv);
		sinfo->signal = priv->rssi;

		sinfo->filled |= BIT(NL80211_STA_INFO_RX_BYTES);
		sinfo->rx_bytes = priv->stats.rx_bytes;
		sinfo->filled |= BIT(NL80211_STA_INFO_RX_PACKETS);
		sinfo->rx_packets = priv->stats.rx_packets;

		sinfo->filled |= BIT(NL80211_STA_INFO_TX_BYTES);
		sinfo->tx_bytes = priv->stats.tx_bytes;
		sinfo->filled |= BIT(NL80211_STA_INFO_TX_PACKETS);
		sinfo->tx_packets = priv->stats.tx_packets;

		sinfo->filled |= BIT(NL80211_STA_INFO_TX_FAILED);
		sinfo->tx_failed = priv->stats.tx_dropped;
	}

	return 0;
}

static int esp_cfg80211_get_tx_power(struct wiphy *wiphy,
				     struct wireless_dev *wdev,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0))
				     int radio_idx,
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0))
				     unsigned int link_id,
#endif
				     int *dbm)
{
	struct esp_wifi_device *priv = NULL;

	int ret;

	if (!wiphy || !wdev || !dbm || !wdev->netdev) {
		esp_info("%u invalid input\n", __LINE__);
		return -EINVAL;
	}

	esp_dbg("\n");
	priv = netdev_priv(wdev->netdev);

	if (!priv || !priv->adapter) {
		esp_err("Empty priv\n");
		return -EINVAL;
	}

	/* cfg80211 may query TX power while unregistering the wdev.  The
	 * interface has already been deinitialized at that point, so return the
	 * last value instead of queueing a command into a tearing-down driver. */
	if (!test_bit(ESP_INTERFACE_INITIALIZED, &priv->priv_flags) ||
	    !test_bit(ESP_INIT_DONE, &priv->adapter->state_flags) ||
	    test_bit(ESP_ALLOW_RECONSTRUCT, &priv->adapter->state_flags) ||
	    test_bit(ESP_CLEANUP_IN_PROGRESS, &priv->adapter->state_flags)) {
		*dbm = esp_pwr_to_dbm(priv->tx_pwr);
		return 0;
	}
	/* Update Tx power from firmware */
	ret = cmd_get_tx_power(priv);
	if (ret)
		return ret;

	*dbm = esp_pwr_to_dbm(priv->tx_pwr);

	return 0;
}

static int esp_cfg80211_set_wiphy_params(struct wiphy *wiphy,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0))
					 int radio_idx,
#endif
					 u32 changed)
{
	esp_dbg("\n");
	return 0;
}

static int esp_cfg80211_set_txq_params(struct wiphy *wiphy, struct net_device *ndev,
			      struct ieee80211_txq_params *params)
{
	esp_dbg("\n");
	return 0;
}


static int esp_set_ies(struct esp_wifi_device *priv, struct cfg80211_beacon_data *info)
{
	const u8 *new_buf[5] = {NULL};
	u16 new_len[5] = {0};
	bool update[5] = {false};
	u8 *prep_buf[5] = {NULL};
	int ret = 0;
	int i, j;

	if (!priv || !info)
		return -EINVAL;

#define FIXED_PARAM_LEN 34

	if (info->head && info->head_len > FIXED_PARAM_LEN) {
		update[0] = true;
		new_buf[0] = info->head + FIXED_PARAM_LEN;
		new_len[0] = info->head_len - FIXED_PARAM_LEN;
	}
	if (info->tail && info->tail_len) {
		update[1] = true;
		new_buf[1] = info->tail;
		new_len[1] = info->tail_len;
	}
	if (info->beacon_ies && info->beacon_ies_len) {
		update[2] = true;
		new_buf[2] = info->beacon_ies;
		new_len[2] = info->beacon_ies_len;
	}
	if (info->proberesp_ies && info->proberesp_ies_len) {
		update[3] = true;
		new_buf[3] = info->proberesp_ies;
		new_len[3] = info->proberesp_ies_len;
	}
	if (info->assocresp_ies && info->assocresp_ies_len) {
		update[4] = true;
		new_buf[4] = info->assocresp_ies;
		new_len[4] = info->assocresp_ies_len;
	}

	/* Allocate all cache buffers upfront before modifying firmware */
	for (i = 0; i < 5; i++) {
		if (!update[i] || !new_len[i])
			continue;
		prep_buf[i] = kmemdup(new_buf[i], new_len[i], GFP_KERNEL);
		if (!prep_buf[i]) {
			while (--i >= 0)
				kfree(prep_buf[i]);
			return -ENOMEM;
		}
	}

	for (i = 0; i < 5; i++) {
		if (!update[i])
			continue;
		ret = cmd_set_ie(priv, i, new_buf[i], new_len[i]);
		if (ret) {
			esp_err("esp_set_ies failed for type %d (ret=%d)\n", i, ret);
			for (j = 0; j < 5; j++)
				kfree(prep_buf[j]);
			return ret;
		}
	}

	for (i = 0; i < 5; i++) {
		if (update[i]) {
			kfree(priv->ap_ie_buf[i]);
			priv->ap_ie_buf[i] = prep_buf[i];
			priv->ap_ie_len[i] = new_len[i];
		}
	}

	return 0;
}

#define ESP_BEACON_PROBE_HEAD_FIXED_LEN 34

static int esp_change_ap_ies(struct esp_wifi_device *priv,
			     struct cfg80211_beacon_data *info)
{
	static const u8 empty_ie;
	const u8 *new_buf[5] = {NULL};
	u16 new_len[5] = {0};
	bool update[5] = {false};
	bool committed[5] = {false};
	u8 *prep_buf[5] = {NULL};
	int ret = 0;
	int i, j;

	if (!priv || !info)
		return -EINVAL;

	/* Validate all parameters before executing any command */
	if (info->head) {
		if (info->head_len < ESP_BEACON_PROBE_HEAD_FIXED_LEN)
			return -EINVAL;
		update[0] = true;
		if (info->head_len > ESP_BEACON_PROBE_HEAD_FIXED_LEN) {
			new_buf[0] = info->head + ESP_BEACON_PROBE_HEAD_FIXED_LEN;
			new_len[0] = info->head_len - ESP_BEACON_PROBE_HEAD_FIXED_LEN;
		}
	} else if (info->head_len) {
		return -EINVAL;
	}

	if (info->tail) {
		update[1] = true;
		new_buf[1] = info->tail;
		new_len[1] = info->tail_len;
	} else if (info->tail_len) {
		return -EINVAL;
	}

	if (info->beacon_ies) {
		update[2] = true;
		new_buf[2] = info->beacon_ies;
		new_len[2] = info->beacon_ies_len;
	} else if (info->beacon_ies_len) {
		return -EINVAL;
	}

	if (info->proberesp_ies) {
		update[3] = true;
		new_buf[3] = info->proberesp_ies;
		new_len[3] = info->proberesp_ies_len;
	} else if (info->proberesp_ies_len) {
		return -EINVAL;
	}

	if (info->assocresp_ies) {
		update[4] = true;
		new_buf[4] = info->assocresp_ies;
		new_len[4] = info->assocresp_ies_len;
	} else if (info->assocresp_ies_len) {
		return -EINVAL;
	}

	/* Allocate all cache buffers upfront before modifying firmware */
	for (i = 0; i < 5; i++) {
		if (!update[i] || !new_len[i])
			continue;
		prep_buf[i] = kmemdup(new_buf[i], new_len[i], GFP_KERNEL);
		if (!prep_buf[i]) {
			while (--i >= 0)
				kfree(prep_buf[i]);
			return -ENOMEM;
		}
	}

	/* Execute updates sequentially with checked rollback on failure */
	for (i = 0; i < 5; i++) {
		if (!update[i])
			continue;
		ret = cmd_set_ie(priv, i, new_len[i] ? new_buf[i] : &empty_ie, new_len[i]);
		if (ret) {
			esp_err("Failed to set AP IE type %d ret=%d, rolling back\n", i, ret);
			while (--i >= 0) {
				int rb_ret;
				if (!committed[i])
					continue;
				rb_ret = cmd_set_ie(priv, i,
						    priv->ap_ie_len[i] ? priv->ap_ie_buf[i] : &empty_ie,
						    priv->ap_ie_len[i]);
				if (rb_ret) {
					esp_err("change_beacon rollback failed for type %d (ret=%d), scheduling recovery\n", i, rb_ret);
					if (priv->adapter) {
						esp_schedule_fw_reset_recovery(priv->adapter);
						esp_request_firmware_restart(priv->adapter);
					}
				}
			}
			for (j = 0; j < 5; j++)
				kfree(prep_buf[j]);
			return ret;
		}
		committed[i] = true;
	}

	/* Commit new IEs to local cache upon full success */
	for (i = 0; i < 5; i++) {
		if (update[i]) {
			kfree(priv->ap_ie_buf[i]);
			priv->ap_ie_buf[i] = prep_buf[i];
			priv->ap_ie_len[i] = new_len[i];
		}
	}
	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0))
static int esp_cfg80211_change_beacon(struct wiphy *wiphy, struct net_device *ndev,
				      struct cfg80211_ap_update *params)
{
	struct cfg80211_beacon_data *info;
#else
static int esp_cfg80211_change_beacon(struct wiphy *wiphy, struct net_device *ndev,
				      struct cfg80211_beacon_data *info)
{
#endif
	struct esp_wifi_device *priv;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0))
	if (!params)
		return -EINVAL;
	info = &params->beacon;
#endif
	if (!wiphy || !ndev || !info)
		return -EINVAL;
	priv = netdev_priv(ndev);
	if (!priv || priv->if_type != ESP_AP_IF)
		return -EINVAL;

	return esp_change_ap_ies(priv, info);
}

static int esp_clear_ap_ies(struct esp_wifi_device *priv)
{
	/* Never pass NULL to memcpy(), even for a zero-length SET_IE. The firmware
	 * interprets zero length as removal. All slots are part of AP transaction
	 * state: record the first failure but attempt every cleanup so a retry has
	 * the smallest possible stale-state surface. */
	static const u8 empty_ie;
	int first = 0;
	int ret;
	int i;

	for (i = 0; i < 5; i++) {
		kfree(priv->ap_ie_buf[i]);
		priv->ap_ie_buf[i] = NULL;
		priv->ap_ie_len[i] = 0;
	}

#define ESP_CLEAR_AP_IE(_type) do { \
	ret = cmd_set_ie(priv, (_type), &empty_ie, 0); \
	if (ret && !first) \
		first = ret; \
} while (0)

	ESP_CLEAR_AP_IE(IE_BEACON_PROBE_HEAD);
	ESP_CLEAR_AP_IE(IE_BEACON_PROBE_TAIL);
	ESP_CLEAR_AP_IE(IE_BEACON);
	ESP_CLEAR_AP_IE(IE_PROBE_RESP);
	ESP_CLEAR_AP_IE(IE_ASSOC_RESP);

#undef ESP_CLEAR_AP_IE
	return first;
}

static int esp_cfg80211_start_ap(struct wiphy *wiphy, struct net_device *dev,
				 struct cfg80211_ap_settings *info)
{
	struct esp_wifi_device *priv;
	struct ieee80211_mgmt *mgmt;
	struct esp_ap_config ap_config = {0};
	u8 *ies;
	int ret;
	int cleanup_ret = 0;
	int stop_ret = 0;
	int i;

	if (!wiphy || !dev || !info)
		return -EINVAL;

	priv = netdev_priv(dev);
	if (!priv || priv->if_type != ESP_AP_IF)
		return -EINVAL;

	/* stop_ap() leaves firmware in STA mode. A subsequent start_ap() on the
	 * same cfg80211 interface does not run change_iface(), so restore APSTA
	 * before touching AP application IEs. */
	ret = cmd_set_mode(priv, esp_get_mode_from_iface_type(priv->if_type));
	if (ret) {
		esp_err("AP start: mode restore failed: %d\n", ret);
		return ret;
	}

	/* Stale application IEs from an earlier AP incarnation are part of the
	 * transaction. In particular esp_set_ies() does not rewrite HEAD when the
	 * new beacon has no variable head IEs, so a failed clear cannot be ignored. */
	ret = esp_clear_ap_ies(priv);
	if (ret) {
		esp_err("AP start: previous IE cleanup failed: %d\n", ret);
		goto rollback;
	}

	/* IE staging is part of AP creation, not a best-effort side effect. */
	ret = esp_set_ies(priv, &info->beacon);
	if (ret) {
		esp_err("AP start: IE staging failed: %d\n", ret);
		goto rollback;
	}

	ap_config.beacon_interval = info->beacon_interval;

	if (!info->beacon.head ||
	    info->beacon.head_len < offsetof(struct ieee80211_mgmt,
					       u.beacon.variable)) {
		ret = -EINVAL;
		goto rollback;
	}
	mgmt = (struct ieee80211_mgmt *)info->beacon.head;
	ies = mgmt->u.beacon.variable;
	if (ies > info->beacon.head + info->beacon.head_len) {
		ret = -EINVAL;
		goto rollback;
	}

	if (!info->ssid || !info->ssid_len ||
	    info->ssid_len > sizeof(ap_config.ssid)) {
		ret = -EINVAL;
		goto rollback;
	}
	memcpy(ap_config.ssid, info->ssid, info->ssid_len);
	ap_config.ssid_len = info->ssid_len;
	if (info->hidden_ssid != NL80211_HIDDEN_SSID_NOT_IN_USE)
		ap_config.ssid_hidden = 1;

	if (info->inactivity_timeout)
		ap_config.inactivity_timeout = info->inactivity_timeout;

	if (info->chandef.chan) {
		for (i = 0; i < ARRAY_SIZE(esp_channels_2ghz); i++) {
			if (esp_channels_2ghz[i].center_freq ==
			    info->chandef.chan->center_freq) {
				ap_config.channel = esp_channels_2ghz[i].hw_value;
				break;
			}
		}
		if (!ap_config.channel && wiphy->bands[NL80211_BAND_5GHZ]) {
			for (i = 0; i < ARRAY_SIZE(esp_channels_5ghz); i++) {
				if (esp_channels_5ghz[i].center_freq ==
				    info->chandef.chan->center_freq) {
					ap_config.channel = esp_channels_5ghz[i].hw_value;
					break;
				}
			}
		}
	}
	if (!ap_config.channel)
		ap_config.channel = 6;

	ap_config.privacy = info->privacy;
	ret = cmd_set_ap_config(priv, &ap_config);
	if (ret)
		goto rollback;

	esp_port_open(priv);
	return 0;

rollback:
	esp_err("AP start rollback: %d\n", ret);
	cleanup_ret = esp_clear_ap_ies(priv);
	stop_ret = cmd_stop_ap(priv);
	if (cleanup_ret || stop_ret) {
		esp_err("AP start rollback failed (ie_ret=%d stop_ret=%d), scheduling recovery\n",
			cleanup_ret, stop_ret);
		if (priv->adapter) {
			esp_schedule_fw_reset_recovery(priv->adapter);
			esp_request_firmware_restart(priv->adapter);
		}
	}
	esp_port_close(priv);
	return ret;
}

static int esp_cfg80211_stop_ap(struct wiphy *wiphy, struct net_device *dev
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0))
				, unsigned int link_id
#endif
				)
{
	struct esp_wifi_device *priv;
	int ret;
	int ie_ret;

	if (!wiphy || !dev)
		return -EINVAL;
	priv = netdev_priv(dev);
	if (!priv || priv->if_type != ESP_AP_IF)
		return -EINVAL;

	/* ESP_MAX_INTERFACE is currently one. Transitioning APSTA -> STA is the
	 * firmware-backed AP teardown for this single exposed interface. */
	ret = cmd_stop_ap(priv);
	if (ret)
		return ret;

	ie_ret = esp_clear_ap_ies(priv);
	if (ie_ret) {
		esp_err("AP stop: IE cleanup failed: %d, scheduling recovery\n", ie_ret);
		if (priv->adapter) {
			esp_schedule_fw_reset_recovery(priv->adapter);
			esp_request_firmware_restart(priv->adapter);
		}
	}
	esp_port_close(priv);
	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0))
static void esp_cfg80211_mgmt_frame_registrations(struct wiphy *wiphy,
						  struct wireless_dev *wdev,
						  struct mgmt_frame_regs *upd)
{
	/* We will forward all the mgmt frame to userspace by default */
}
#endif

static int esp_cfg80211_probe_client(struct wiphy *wiphy, struct net_device *dev,
                                  const u8 *peer, u64 *cookie)
{
	return -EOPNOTSUPP;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))
static int esp_cfg80211_del_station(struct wiphy *wiphy, struct wireless_dev *wdev,
					struct station_del_parameters *params)
{
	struct net_device *dev = wdev->netdev;
#else
static int esp_cfg80211_del_station(struct wiphy *wiphy, struct net_device *dev,
					struct station_del_parameters *params)
{
#endif
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !dev) {
		esp_err("%u invalid params\n", __LINE__);
		return -EINVAL;
	}

	priv = netdev_priv(dev);
	if (!priv) {
		esp_err("empty priv\n");
		return -EINVAL;
	}
	if (priv->if_type == ESP_AP_IF)
		return cmd_disconnect_request(priv, params->reason_code, params->mac, DISCONNECT_TYPE_DEAUTH);

	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))
static int esp_cfg80211_add_station(struct wiphy *wiphy, struct wireless_dev *wdev,
				    const u8 *mac,
				    struct station_parameters *params)
{
	struct net_device *dev = wdev->netdev;
#else
static int esp_cfg80211_add_station(struct wiphy *wiphy, struct net_device *dev,
				    const u8 *mac,
				    struct station_parameters *params)
{
#endif
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !dev) {
		esp_err("%u invalid params\n", __LINE__);
		return -EINVAL;
	}

	priv = netdev_priv(dev);
	if (!priv) {
		esp_err("empty priv\n");
		return -EINVAL;
	}

	if (priv->if_type == ESP_AP_IF)
		return cmd_add_station(priv, mac, params, false);

	return 0;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0))
static int esp_cfg80211_change_station(struct wiphy *wiphy,
				       struct wireless_dev *wdev, const u8 *mac,
				       struct station_parameters *params)
{
	struct net_device *dev = wdev->netdev;
#else
static int esp_cfg80211_change_station(struct wiphy *wiphy,
				       struct net_device *dev, const u8 *mac,
				       struct station_parameters *params)
{
#endif
	struct esp_wifi_device *priv = NULL;

	if (!wiphy || !dev || !mac || !params) {
		esp_err("%u invalid params\n", __LINE__);
		return -EINVAL;
	}

	priv = netdev_priv(dev);
	if (!priv) {
		esp_err("empty priv\n");
		return -EINVAL;
	}
	if (priv->if_type == ESP_AP_IF) {
		int ret = cfg80211_check_station_change(wiphy, params,
							CFG80211_STA_AP_MLME_CLIENT);
		if (ret)
			return ret;
		return cmd_add_station(priv, mac, params, true);
	}

	if (priv->if_type == ESP_STA_IF &&
	    (params->sta_flags_mask & BIT(NL80211_STA_FLAG_AUTHORIZED))) {
		bool authorized = !!(params->sta_flags_set &
				 BIT(NL80211_STA_FLAG_AUTHORIZED));

		esp_dbg("STA_PORT_AUTH_RX "MACSTR" authorized=%u mask=0x%x set=0x%x\n",
			 MAC2STR(mac), authorized, params->sta_flags_mask,
			 params->sta_flags_set);
		return cmd_sta_set_authorized(priv, mac, authorized);
	}

	return 0;
}

static struct cfg80211_ops esp_cfg80211_ops = {
#if 0
	.add_virtual_intf = esp_cfg80211_add_iface,
	.del_virtual_intf = esp_cfg80211_del_iface,
#endif
	.change_virtual_intf = esp_cfg80211_change_iface,
	.scan = esp_cfg80211_scan,
	/*.connect = esp_cfg80211_connect,*/
	.disconnect = esp_cfg80211_disconnect,
	.add_key = esp_cfg80211_add_key,
	.del_key = esp_cfg80211_del_key,
	.set_default_key = esp_cfg80211_set_default_key,
	.set_default_mgmt_key = esp_cfg80211_set_default_mgmt_key,
	.mgmt_tx = esp_cfg80211_mgmt_tx,
	.auth = esp_cfg80211_authenticate,
	.deauth = esp_cfg80211_deauth,
	.disassoc = esp_cfg80211_disassoc,
	.assoc = esp_cfg80211_associate,
	.suspend = esp_cfg80211_suspend,
	.resume = esp_cfg80211_resume,
	.set_wakeup = esp_cfg80211_set_wakeup,
	.set_tx_power = esp_cfg80211_set_tx_power,
	.get_tx_power = esp_cfg80211_get_tx_power,
	.set_wiphy_params = esp_cfg80211_set_wiphy_params,
	.set_txq_params = esp_cfg80211_set_txq_params,
	.change_beacon = esp_cfg80211_change_beacon,
	.start_ap = esp_cfg80211_start_ap,
	.stop_ap = esp_cfg80211_stop_ap,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0))
	.update_mgmt_frame_registrations = esp_cfg80211_mgmt_frame_registrations,
#endif
	.probe_client = esp_cfg80211_probe_client,
	.del_station = esp_cfg80211_del_station,
	.add_station = esp_cfg80211_add_station,
	.change_station = esp_cfg80211_change_station,
	.get_station = esp_cfg80211_get_station,
	.set_ap_chanwidth = esp_cfg80211_set_ap_chanwidth
};

static const struct ieee80211_txrx_stypes
esp_default_mgmt_stypes[NUM_NL80211_IFTYPES] = {
	[NL80211_IFTYPE_AP] = {
		.tx = 0xffff,
		.rx = BIT(IEEE80211_STYPE_ASSOC_REQ >> 4) |
			BIT(IEEE80211_STYPE_REASSOC_REQ >> 4) |
			BIT(IEEE80211_STYPE_PROBE_REQ >> 4) |
			BIT(IEEE80211_STYPE_DISASSOC >> 4) |
			BIT(IEEE80211_STYPE_AUTH >> 4) |
			BIT(IEEE80211_STYPE_DEAUTH >> 4) |
			BIT(IEEE80211_STYPE_ACTION >> 4),
	},
};

static void esp_reg_notifier(struct wiphy *wiphy,
			     struct regulatory_request *request)
{
	struct esp_device *esp_dev;
	struct esp_adapter *adapter;
	struct esp_wifi_device *priv;
	int ret;

	if (!wiphy || !request)
		return;

	adapter = esp_get_adapter();
	if (!adapter)
		return;
	if (test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags) ||
	    test_bit(ESP_CLEANUP_IN_PROGRESS, &adapter->state_flags) ||
	    !test_bit(ESP_INIT_DONE, &adapter->state_flags))
		return;

	esp_dev = wiphy_priv(wiphy);
	if (!esp_dev || !esp_dev->adapter)
		return;
	priv = esp_dev->adapter->priv[0];
	if (!priv)
		return;

	switch (request->initiator) {
	case NL80211_REGDOM_SET_BY_DRIVER:
	case NL80211_REGDOM_SET_BY_CORE:
	case NL80211_REGDOM_SET_BY_USER:
	case NL80211_REGDOM_SET_BY_COUNTRY_IE:
		break;
	default:
		return;
	}

	/* Do not pre-commit alpha2. Identical callbacks are suppressed only
	 * after firmware has acknowledged the country-code update. */
	if (!memcmp(request->alpha2, priv->country_code, 2))
		return;

	ret = cmd_set_reg_domain(priv, request->alpha2);
	if (ret)
		esp_err("Regulatory domain %c%c apply failed: %d\n",
			request->alpha2[0], request->alpha2[1], ret);
}

int esp_add_wiphy(struct esp_adapter *adapter)
{
	struct wiphy *wiphy;
	struct esp_device *esp_dev;
	int ret = 0;

	if (!adapter) {
		esp_info("adapter not yet initialized\n");
		return -EINVAL;
	}

	wiphy = wiphy_new(&esp_cfg80211_ops, sizeof(struct esp_device));

	if (!wiphy) {
		esp_err("Failed to create wiphy\n");
		return -EFAULT;
	}

	adapter->wiphy = wiphy;

	esp_dev = wiphy_priv(wiphy);
	esp_dev->wiphy = wiphy;
	esp_dev->adapter = adapter;

	esp_dev->dev = adapter->dev;

	set_wiphy_dev(wiphy, esp_dev->dev);

	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
#ifdef CONFIG_AP_MODE
	wiphy->interface_modes |= BIT(NL80211_IFTYPE_AP);
#endif
	wiphy->bands[NL80211_BAND_2GHZ] = &esp_wifi_bands_2ghz;
	if (adapter->chipset == ESP_FIRMWARE_CHIP_ESP32C5) {
		wiphy->bands[NL80211_BAND_5GHZ] = &esp_wifi_bands_5ghz;
	}
	/* Initialize cipher suits */
	if (adapter->chipset == ESP_FIRMWARE_CHIP_ESP32C3 ||
	    adapter->chipset == ESP_FIRMWARE_CHIP_ESP32S3 ||
	    adapter->chipset == ESP_FIRMWARE_CHIP_ESP32C5 ||
	    adapter->chipset == ESP_FIRMWARE_CHIP_ESP32C6 ||
	    adapter->chipset == ESP_FIRMWARE_CHIP_ESP32C61) {
		wiphy->cipher_suites = esp_cipher_suites_new;
		wiphy->n_cipher_suites = ARRAY_SIZE(esp_cipher_suites_new);
	} else {
		wiphy->cipher_suites = esp_cipher_suites;
		wiphy->n_cipher_suites = ARRAY_SIZE(esp_cipher_suites);
	}

	/* ESP scan supports at most 1 SSID, no probe request IEs, and no scheduled scans */
	wiphy->max_scan_ssids = 1;
	/*	wiphy->max_match_sets = 10;*/
	wiphy->max_scan_ie_len = 0;
	wiphy->max_sched_scan_ssids = 0;
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
#ifdef CONFIG_PM
	wiphy->wowlan = &esp_wowlan_support;
#endif
	wiphy->mgmt_stypes = esp_default_mgmt_stypes;

	/* Advertise SAE support */
	wiphy->features |= NL80211_FEATURE_SAE;

	wiphy->reg_notifier = esp_reg_notifier;

	/* set caps */
	wiphy->flags |= WIPHY_FLAG_AP_PROBE_RESP_OFFLOAD;
	wiphy->flags |= WIPHY_FLAG_REPORTS_OBSS;
	//wiphy->features |= NL80211_CMD_PROBE_CLIENT;
	wiphy->features |= NL80211_FEATURE_SK_TX_STATUS;

	ret = wiphy_register(wiphy);
	if (ret) {
		esp_err("Failed to register wiphy: %d\n", ret);
		adapter->wiphy = NULL;
		wiphy_free(wiphy);
	}

	return ret;
}

int esp_remove_wiphy(struct esp_adapter *adapter)
{
	if (adapter && adapter->wiphy) {
		wiphy_unregister(adapter->wiphy);
		wiphy_free(adapter->wiphy);
		adapter->wiphy = NULL;
	}

	return 0;
}

int esp_mark_disconnect(struct esp_wifi_device *priv, uint16_t reason, uint8_t locally_disconnect)
{
	if (priv && priv->ndev && wireless_dev_current_bss_exists(&priv->wdev))
		CFG80211_DISCONNECTED(priv->ndev, reason, NULL, 0, locally_disconnect, GFP_KERNEL);

	return 0;
}

int esp_mark_scan_done_and_disconnect(struct esp_wifi_device *priv, uint8_t locally_disconnect)
{

	if (!priv)
		return -EINVAL;

	if (priv->wdev.iftype != NL80211_IFTYPE_STATION)
		return 0;

	ESP_MARK_SCAN_DONE(priv, true);
	esp_wifi_put_bss(priv);
	return esp_mark_disconnect(priv, 0, locally_disconnect);
}
