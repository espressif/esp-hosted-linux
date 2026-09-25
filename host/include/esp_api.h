// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 */
#ifndef _esp_api__h_
#define _esp_api__h_

#include "esp.h"
#include "esp_utils.h"

#define RET_ON_FAIL(x)                                                \
	do {                                                          \
		int ret = (x);                                        \
		if (!!(ret)) {                                        \
		    esp_err("%s failure, ret: %d\n", #x, ret);        \
		    return ret;                                       \
		}                                                     \
	} while (0);                                                  \

int esp_add_card(struct esp_adapter *adapter);
int esp_remove_card(struct esp_adapter *adapter, bool notify_fw);
void esp_process_new_packet_intr(struct esp_adapter *adapter);
struct esp_adapter *esp_get_adapter(void);
struct esp_wifi_device *get_priv_from_payload_header(struct esp_payload_header *header);
int esp_send_packet(struct esp_adapter *adapter, struct sk_buff *skb);
u8 esp_is_bt_supported_over_sdio(u32 cap);
void esp_tx_pause(struct esp_wifi_device *priv);
void esp_tx_resume(struct esp_wifi_device *priv);
void esp_init_priv(struct net_device *ndev);
void esp_port_open(struct esp_wifi_device *priv);
void esp_port_close(struct esp_wifi_device *priv);
void esp_remove_network_interfaces(struct esp_adapter *adapter);
void print_capabilities(u32 cap);
void process_capabilities(struct esp_adapter *adapter);
int esp_is_tx_queue_paused(struct esp_wifi_device *priv);
int esp_deinit_module(struct esp_adapter *adapter);
int esp_validate_chipset(struct esp_adapter *adapter, u8 chipset);
int esp_adjust_spi_clock(struct esp_adapter *adapter, u8 spi_clk_mhz);
void process_test_capabilities(u32 raw_tp_mode);
int esp_init_raw_tp(struct esp_adapter *adapter);
bool esp_is_valid_hardware_id(int hardware_id);
char *esp_get_hardware_name(int hardware_id);
int generate_slave_intr(void *context, u8 data);
int esp_start_ota(struct esp_adapter *adapter, char *ota_file);
void esp_schedule_recovery(struct esp_adapter *adapter,
		unsigned int delay_ms, bool restart_timer, bool fw_reset);
void esp_schedule_transport_recovery(struct esp_adapter *adapter);
void esp_schedule_fw_reset_recovery(struct esp_adapter *adapter);
void esp_request_firmware_restart(struct esp_adapter *adapter);
bool esp_host_reset_available(void);

/* Legacy main.c acquires resetpin through gpio_request(..., "sysfs"). Route
 * GPIO operations through an ownership guard so a failed request cannot still
 * drive a GPIO and repeated firmware recovery does not re-request the line.
 * Non-reset GPIO labels retain native gpiolib behavior. */
#undef gpio_request
#undef gpio_is_valid
#undef gpio_direction_output
#undef gpio_direction_input
#undef gpio_set_value
#undef gpio_free
#define gpio_request(_gpio, _label) \
	esp_gpio_request_guard((_gpio), (_label))
#define gpio_is_valid(_gpio) \
	esp_gpio_is_valid_guard((_gpio))
#define gpio_direction_output(_gpio, _value) \
	esp_gpio_direction_output_guard((_gpio), (_value))
#define gpio_direction_input(_gpio) \
	esp_gpio_direction_input_guard((_gpio))
#define gpio_set_value(_gpio, _value) \
	esp_gpio_set_value_guard((_gpio), (_value))
#define gpio_free(_gpio) \
	esp_gpio_free_guard((_gpio))

#endif
