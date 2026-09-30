/*
 * Hosted-ng replacements for IDF wpa_supplicant symbols.
 *
 * Linux runs wpa_supplicant / hostapd (including SAE and OWE). The chip
 * forwards EAPOL/mgmt and does data-plane CCMP/GMAC after the host installs
 * keys. Providing these symbols stops the linker from pulling the IDF 4-way /
 * SAE / AP authenticator objects.
 *
 * Kconfig WPA3-SAE and OWE stay enabled so the Wi-Fi blob keeps those
 * feature bits. Do not turn those options off.
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_wifi_crypto_types.h"
#include "esp_wpa.h"
#include "esp_wifi_types.h"
#include "esp_random.h"
#include <sys/time.h>
#include <unistd.h>

/* Matches network_adapter/main/esp_wifi_driver.h — avoid including that
 * header here (it needs wpa_supplicant u8 typedefs). */
typedef struct {
    int proto;
    int pairwise_cipher;
    int group_cipher;
    int key_mgmt;
    int capabilities;
    size_t num_pmkid;
    const uint8_t *pmkid;
    int mgmt_group_cipher;
    uint8_t rsnxe_capa;
} wifi_wpa_ie_t;


/* From cmd.c — overwrites IDF callbacks after esp_wifi_init(). */
int esp_wifi_unregister_wpa_cb_internal(void);

#define WLAN_EID_RSN            48
#define WLAN_EID_VENDOR         221
#define WLAN_EID_RSNX           244

#define WPA_PROTO_WPA           (1 << 0)
#define WPA_PROTO_RSN           (1 << 1)

#define WPA_CIPHER_NONE         (1 << 0)
#define WPA_CIPHER_TKIP         (1 << 1)
#define WPA_CIPHER_CCMP         (1 << 3)
#define WPA_CIPHER_AES_128_CMAC (1 << 5)
#define WPA_CIPHER_WEP40        (1 << 7)
#define WPA_CIPHER_WEP104       (1 << 8)
#define WPA_CIPHER_GCMP         (1 << 11)
#define WPA_CIPHER_GCMP_256     (1 << 12)
#define WPA_CIPHER_BIP_GMAC_128 (1 << 13)
#define WPA_CIPHER_BIP_GMAC_256 (1 << 14)

#define WPA_KEY_MGMT_IEEE8021X        (1 << 0)
#define WPA_KEY_MGMT_PSK              (1 << 1)
#define WPA_KEY_MGMT_IEEE8021X_SHA256 (1 << 7)
#define WPA_KEY_MGMT_PSK_SHA256       (1 << 8)
#define WPA_KEY_MGMT_SAE              (1 << 10)
#define WPA_KEY_MGMT_FT_SAE           (1 << 11)
#define WPA_KEY_MGMT_OWE              (1 << 22)
#define WPA_KEY_MGMT_SAE_EXT_KEY      (1 << 26)

#define RSN_SEL(a, b, c, d) \
    ((((uint32_t)(a)) << 24) | (((uint32_t)(b)) << 16) | \
     (((uint32_t)(c)) << 8) | (uint32_t)(d))
static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}


static unsigned suite_to_cipher(uint32_t suite)
{
    switch (suite) {
    case RSN_SEL(0x00, 0x0f, 0xac, 0):
    case RSN_SEL(0x00, 0x50, 0xf2, 0):
        return WPA_CIPHER_NONE;
    case RSN_SEL(0x00, 0x0f, 0xac, 1):
    case RSN_SEL(0x00, 0x50, 0xf2, 1):
        return WPA_CIPHER_WEP40;
    case RSN_SEL(0x00, 0x0f, 0xac, 2):
    case RSN_SEL(0x00, 0x50, 0xf2, 2):
        return WPA_CIPHER_TKIP;
    case RSN_SEL(0x00, 0x0f, 0xac, 4):
    case RSN_SEL(0x00, 0x50, 0xf2, 4):
        return WPA_CIPHER_CCMP;
    case RSN_SEL(0x00, 0x0f, 0xac, 5):
    case RSN_SEL(0x00, 0x50, 0xf2, 5):
        return WPA_CIPHER_WEP104;
    case RSN_SEL(0x00, 0x0f, 0xac, 6):
        return WPA_CIPHER_AES_128_CMAC;
    case RSN_SEL(0x00, 0x0f, 0xac, 8):
        return WPA_CIPHER_GCMP;
    case RSN_SEL(0x00, 0x0f, 0xac, 9):
        return WPA_CIPHER_GCMP_256;
    case RSN_SEL(0x00, 0x0f, 0xac, 11):
        return WPA_CIPHER_BIP_GMAC_128;
    case RSN_SEL(0x00, 0x0f, 0xac, 12):
        return WPA_CIPHER_BIP_GMAC_256;
    default:
        return 0;
    }
}

static unsigned suite_to_key_mgmt(uint32_t suite)
{
    switch (suite) {
    case RSN_SEL(0x00, 0x0f, 0xac, 1):
    case RSN_SEL(0x00, 0x50, 0xf2, 1):
        return WPA_KEY_MGMT_IEEE8021X;
    case RSN_SEL(0x00, 0x0f, 0xac, 2):
    case RSN_SEL(0x00, 0x50, 0xf2, 2):
        return WPA_KEY_MGMT_PSK;
    case RSN_SEL(0x00, 0x0f, 0xac, 5):
        return WPA_KEY_MGMT_IEEE8021X_SHA256;
    case RSN_SEL(0x00, 0x0f, 0xac, 6):
        return WPA_KEY_MGMT_PSK_SHA256;
    case RSN_SEL(0x00, 0x0f, 0xac, 8):
        return WPA_KEY_MGMT_SAE;
    case RSN_SEL(0x00, 0x0f, 0xac, 9):
        return WPA_KEY_MGMT_FT_SAE;
    case RSN_SEL(0x00, 0x0f, 0xac, 18):
        return WPA_KEY_MGMT_OWE;
    case RSN_SEL(0x00, 0x0f, 0xac, 24):
        return WPA_KEY_MGMT_SAE_EXT_KEY;
    default:
        return 0;
    }
}

static wifi_cipher_type_t cipher_to_public(unsigned wpa_cipher)
{
    switch (wpa_cipher) {
    case WPA_CIPHER_NONE:
        return WIFI_CIPHER_TYPE_NONE;
    case WPA_CIPHER_WEP40:
        return WIFI_CIPHER_TYPE_WEP40;
    case WPA_CIPHER_WEP104:
        return WIFI_CIPHER_TYPE_WEP104;
    case WPA_CIPHER_TKIP:
        return WIFI_CIPHER_TYPE_TKIP;
    case WPA_CIPHER_CCMP:
        return WIFI_CIPHER_TYPE_CCMP;
    case WPA_CIPHER_CCMP | WPA_CIPHER_TKIP:
        return WIFI_CIPHER_TYPE_TKIP_CCMP;
    case WPA_CIPHER_AES_128_CMAC:
        return WIFI_CIPHER_TYPE_AES_CMAC128;
    case WPA_CIPHER_BIP_GMAC_128:
        return WIFI_CIPHER_TYPE_AES_GMAC128;
    case WPA_CIPHER_BIP_GMAC_256:
        return WIFI_CIPHER_TYPE_AES_GMAC256;
    case WPA_CIPHER_GCMP:
        return WIFI_CIPHER_TYPE_GCMP;
    case WPA_CIPHER_GCMP_256:
        return WIFI_CIPHER_TYPE_GCMP256;
    default:
        return WIFI_CIPHER_TYPE_UNKNOWN;
    }
}

static int parse_rsn_or_wpa(const uint8_t *ie, size_t ie_len, int proto,
                            wifi_wpa_ie_t *out)
{
    const uint8_t *pos;
    int left;
    uint16_t count, i;
    unsigned pairwise = 0, akm = 0;

    memset(out, 0, sizeof(*out));
    out->proto = proto;

    if (ie_len < 6 || ie[1] + 2 > (int)ie_len) {
        return -1;
    }

    pos = ie + 2;
    left = ie[1];

    if (proto == WPA_PROTO_WPA) {
        if (left < 6 || get_be32(pos) != RSN_SEL(0x00, 0x50, 0xf2, 1)) {
            return -1;
        }
        pos += 4;
        left -= 4;
    }

    if (left < 2 || get_le16(pos) != 1) {
        return -1;
    }
    pos += 2;
    left -= 2;

    if (left < 4) {
        return -1;
    }
    out->group_cipher = cipher_to_public(suite_to_cipher(get_be32(pos)));
    pos += 4;
    left -= 4;

    if (left < 2) {
        return -1;
    }
    count = get_le16(pos);
    pos += 2;
    left -= 2;
    if (left < count * 4) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        pairwise |= suite_to_cipher(get_be32(pos));
        pos += 4;
        left -= 4;
    }
    out->pairwise_cipher = cipher_to_public(pairwise);

    if (left < 2) {
        return -1;
    }
    count = get_le16(pos);
    pos += 2;
    left -= 2;
    if (left < count * 4) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        akm |= suite_to_key_mgmt(get_be32(pos));
        pos += 4;
        left -= 4;
    }
    out->key_mgmt = (int)akm;

    if (left >= 2) {
        out->capabilities = get_le16(pos);
        pos += 2;
        left -= 2;
    }

    if (proto == WPA_PROTO_RSN && left >= 2) {
        count = get_le16(pos);
        pos += 2;
        left -= 2;
        if (count && left >= count * 16) {
            out->num_pmkid = count;
            out->pmkid = pos;
            pos += count * 16;
            left -= count * 16;
        }
        if (left >= 4) {
            out->mgmt_group_cipher = cipher_to_public(suite_to_cipher(get_be32(pos)));
        }
    }

    return 0;
}

int wpa_parse_wpa_ie_wrapper(const uint8_t *wpa_ie, size_t wpa_ie_len, wifi_wpa_ie_t *data)
{
    if (!wpa_ie || !data || wpa_ie_len < 2) {
        return -1;
    }

    if (wpa_ie[0] == WLAN_EID_RSN) {
        return parse_rsn_or_wpa(wpa_ie, wpa_ie_len, WPA_PROTO_RSN, data);
    }
    if (wpa_ie[0] == WLAN_EID_RSNX) {
        memset(data, 0, sizeof(*data));
        if (wpa_ie_len < 3) {
            return -1;
        }
        data->rsnxe_capa = wpa_ie[2];
        return 0;
    }
    return parse_rsn_or_wpa(wpa_ie, wpa_ie_len, WPA_PROTO_WPA, data);
}

esp_err_t esp_supplicant_init(void)
{
    /* Callbacks are installed by initialise_wifi() after esp_wifi_init(). */
    return ESP_OK;
}

esp_err_t esp_supplicant_deinit(void)
{
    return esp_wifi_unregister_wpa_cb_internal();
}

esp_err_t esp_supplicant_disable_pmk_caching(bool disable)
{
    (void)disable;
    return ESP_OK;
}

int map_wifi_config_sae_pwe_to_supp(wifi_sae_pwe_method_t sae_pwe_config)
{
    switch (sae_pwe_config) {
    case WPA3_SAE_PWE_HASH_TO_ELEMENT:
        return 1; /* SAE_PWE_HASH_TO_ELEMENT */
    case WPA3_SAE_PWE_BOTH:
    case WPA3_SAE_PWE_UNSPECIFIED:
        return 2; /* SAE_PWE_BOTH */
    case WPA3_SAE_PWE_HUNT_AND_PECK:
    default:
        return 0; /* SAE_PWE_HUNT_AND_PECK */
    }
}
