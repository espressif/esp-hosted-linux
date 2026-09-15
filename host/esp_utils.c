// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 */
#include "utils.h"
#include "esp_utils.h"
#include <linux/gpio.h>
#include <linux/mutex.h>
#include <linux/string.h>

/* main.c historically requests resetpin with label "sysfs" inside every
 * esp_reset() invocation and ignores gpio_request() / direction failures. Keep
 * that legacy call site byte-for-byte stable, but make the GPIO an actual
 * owned and proven resource: acquire once, serialize each complete reset
 * pulse, make repeated requests idempotent, fail closed after any ownership or
 * direction error, and release exactly once. Other GPIO users (SPI
 * handshake/data-ready, etc.) pass different labels and retain native GPIO API
 * semantics. */
static DEFINE_MUTEX(esp_reset_gpio_lock);
static DEFINE_MUTEX(esp_reset_pulse_lock);
static int esp_reset_gpio = -1;
static int esp_rejected_reset_gpio = -1;
static bool esp_reset_gpio_seen;
static bool esp_reset_gpio_owned;
static bool esp_reset_gpio_operational;

/* Avoid including esp_api.h here: it intentionally remaps the legacy GPIO API
 * to these guards. esp_utils.c must call native gpiolib underneath them. */
struct esp_adapter *esp_get_adapter(void);

static bool esp_is_reset_gpio_label(const char *label)
{
	return label && !strcmp(label, "sysfs");
}

int esp_gpio_request_guard(unsigned int gpio, const char *label)
{
	int ret;

	if (!esp_is_reset_gpio_label(label))
		return gpio_request(gpio, label);

	/* gpio_direction_input_guard() releases this after the complete legacy
	 * reset sequence. This prevents two recovery callers from interleaving
	 * output-low/release operations and shortening each other's reset pulse. */
	mutex_lock(&esp_reset_pulse_lock);

	mutex_lock(&esp_reset_gpio_lock);
	if (!esp_reset_gpio_seen) {
		esp_reset_gpio_seen = true;
		esp_reset_gpio = gpio;
		esp_rejected_reset_gpio = -1;
		ret = gpio_request(gpio, label);
		esp_reset_gpio_owned = (ret == 0);
		esp_reset_gpio_operational = (ret == 0);
	} else if (esp_reset_gpio != gpio) {
		/* resetpin is writable as a module parameter. Changing it after the
		 * original line was acquired is unsupported: remember the rejected
		 * candidate so the legacy direction/value calls that follow this
		 * ignored error are also fail-closed. Reload to acquire another pin. */
		esp_rejected_reset_gpio = gpio;
		ret = -EBUSY;
	} else if (esp_reset_gpio_owned && esp_reset_gpio_operational) {
		ret = 0;
	} else {
		ret = -EBUSY;
	}
	mutex_unlock(&esp_reset_gpio_lock);
	return ret;
}

bool esp_gpio_is_valid_guard(int gpio)
{
	bool valid = gpio_is_valid(gpio);

	if (!valid)
		return false;

	mutex_lock(&esp_reset_gpio_lock);
	if (gpio == esp_rejected_reset_gpio ||
	    (esp_reset_gpio_seen && esp_reset_gpio == gpio &&
	     (!esp_reset_gpio_owned || !esp_reset_gpio_operational)))
		valid = false;
	mutex_unlock(&esp_reset_gpio_lock);
	return valid;
}

static bool esp_gpio_change_allowed(unsigned int gpio)
{
	bool allowed = true;

	mutex_lock(&esp_reset_gpio_lock);
	if (gpio == esp_rejected_reset_gpio ||
	    (esp_reset_gpio_seen && esp_reset_gpio == gpio &&
	     (!esp_reset_gpio_owned || !esp_reset_gpio_operational)))
		allowed = false;
	mutex_unlock(&esp_reset_gpio_lock);
	return allowed;
}

static void esp_reset_gpio_mark_failed(unsigned int gpio)
{
	mutex_lock(&esp_reset_gpio_lock);
	if (esp_reset_gpio_seen && esp_reset_gpio == gpio)
		esp_reset_gpio_operational = false;
	mutex_unlock(&esp_reset_gpio_lock);
}

int esp_gpio_direction_output_guard(unsigned int gpio, int value)
{
	int ret;

	if (!esp_gpio_change_allowed(gpio))
		return -EBUSY;
	ret = gpio_direction_output(gpio, value);
	if (ret)
		esp_reset_gpio_mark_failed(gpio);
	return ret;
}

int esp_gpio_direction_input_guard(unsigned int gpio)
{
	bool reset_call;
	int ret;

	mutex_lock(&esp_reset_gpio_lock);
	reset_call = (esp_reset_gpio_seen && esp_reset_gpio == gpio) ||
		     gpio == esp_rejected_reset_gpio;
	mutex_unlock(&esp_reset_gpio_lock);

	if (!esp_gpio_change_allowed(gpio)) {
		ret = -EBUSY;
	} else {
		ret = gpio_direction_input(gpio);
		if (ret)
			esp_reset_gpio_mark_failed(gpio);
	}

	/* Every reset-label request reaches the legacy direction-input call even
	 * when request/output failed; it is therefore the pulse-lock release point. */
	if (reset_call)
		mutex_unlock(&esp_reset_pulse_lock);
	return ret;
}

void esp_gpio_set_value_guard(unsigned int gpio, int value)
{
	if (!esp_gpio_change_allowed(gpio))
		return;
	gpio_set_value(gpio, value);
}

void esp_gpio_free_guard(unsigned int gpio)
{
	struct esp_adapter *adapter = esp_get_adapter();
	int reset_to_free = -1;
	bool swallow = false;
	bool unloading = adapter &&
		test_bit(ESP_DRIVER_UNLOADING, &adapter->state_flags);

	mutex_lock(&esp_reset_gpio_lock);

	/* Once unload is published, recovery is already cancelled. Release the
	 * actually-owned reset line at the first GPIO cleanup opportunity rather
	 * than trusting the writable module parameter still names that line. */
	if (unloading && esp_reset_gpio_owned) {
		reset_to_free = esp_reset_gpio;
		esp_reset_gpio_owned = false;
		esp_reset_gpio_operational = false;
	}

	/* Never free an unowned/rejected resetpin merely because main.c still
	 * calls gpio_free(resetpin) on exit. Native non-reset GPIO frees continue
	 * to pass through normally. */
	if ((esp_reset_gpio_seen && gpio == esp_reset_gpio) ||
	    gpio == esp_rejected_reset_gpio)
		swallow = true;

	mutex_unlock(&esp_reset_gpio_lock);

	if (reset_to_free >= 0)
		gpio_free(reset_to_free);
	if (!swallow && (int)gpio != reset_to_free)
		gpio_free(gpio);
}


int wpa_cipher_to_alg(int cipher)
{
	switch (cipher) {
	case WLAN_CIPHER_SUITE_CCMP:
		return WIFI_WPA_ALG_CCMP;
#ifdef CONFIG_GCMP
	case WLAN_CIPHER_SUITE_GCMP_256:
	case WLAN_CIPHER_SUITE_GCMP:
		return WIFI_WPA_ALG_GCMP;
#endif
	case WLAN_CIPHER_SUITE_TKIP:
		return WIFI_WPA_ALG_TKIP;
	case WLAN_CIPHER_SUITE_WEP104:
		return WIFI_WPA_ALG_WEP104;
	case WLAN_CIPHER_SUITE_WEP40:
		return WIFI_WPA_ALG_WEP40;
	case WLAN_CIPHER_SUITE_AES_CMAC:
		return WIFI_WPA_ALG_IGTK;
	}
	return WIFI_WPA_ALG_NONE;
}

char * esp_chipname_from_id(int chipset_id)
{
	if (chipset_id == ESP_FIRMWARE_CHIP_ESP32)
		return "ESP32";
	if (chipset_id == ESP_FIRMWARE_CHIP_ESP32S2)
		return "ESP32-S2";
	if (chipset_id == ESP_FIRMWARE_CHIP_ESP32S3)
		return "ESP32-S3";
	if (chipset_id == ESP_FIRMWARE_CHIP_ESP32C2)
		return "ESP32-C2";
	if (chipset_id == ESP_FIRMWARE_CHIP_ESP32C3)
		return "ESP32-C3";
	if (chipset_id == ESP_FIRMWARE_CHIP_ESP32C6)
		return "ESP32-C6";
	if (chipset_id == ESP_FIRMWARE_CHIP_ESP32C61)
		return "ESP32-C61";
	if (chipset_id == ESP_FIRMWARE_CHIP_ESP32C5)
		return "ESP32-C5";

	return "Unknown Chip";
}
