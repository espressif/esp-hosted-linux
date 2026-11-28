// SPDX-License-Identifier: GPL-2.0-only
#ifndef ESP_SPI_COMMON_H
#define ESP_SPI_COMMON_H

#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/skbuff.h>
#include "esp.h"
#include "esp_cmd.h"
#include "esp_host_wire_bounds.h"
#include "esp_transport_buf.h"

/* Shared transport-independent buffer alignment and command metadata. */
static inline struct sk_buff *esp_spi_alloc_aligned_skb(u32 len,
							  u32 min_len)
{
	return esp_transport_alloc_skb(len, min_len,
				       esp_transport_dma_alignment());
}

static inline unsigned long esp_spi_gpio_irq_flags(struct gpio_desc *gpio)
{
	return gpiod_is_active_low(gpio) ?
		IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;
}

static inline bool esp_spi_get_cmd_info(struct sk_buff *skb, u8 *cmd_code, u16 *cmd_seq)
{
	struct esp_payload_header *payload_header;
	struct command_header *cmd;
	u16 offset;

	if (!skb || skb->len < sizeof(*payload_header))
		return false;
	payload_header = (struct esp_payload_header *)skb->data;
	if (payload_header->packet_type != PACKET_TYPE_COMMAND_REQUEST)
		return false;
	offset = esp_wire_le16_to_cpu(payload_header->offset);
	if (offset + sizeof(*cmd) > skb->len)
		return false;
	cmd = (struct command_header *)(skb->data + offset);
	if (cmd_code)
		*cmd_code = cmd->cmd_code;
	if (cmd_seq)
		*cmd_seq = esp_wire_le16_to_cpu(cmd->seq_num);
	return true;
}

static inline bool esp_spi_is_stateful_packet(const struct sk_buff *skb)
{
	const struct esp_payload_header *header;

	if (!skb || skb->len < sizeof(*header))
		return false;
	header = (const void *)skb->data;
	return header->if_type == ESP_HCI_IF || header->if_type == ESP_RCP_IF;
}

/* Assert the logical reset level, deassert it and release the line.
 * Callers own reset serialization and choose the board's settling time. */
static inline int esp_spi_pulse_reset_gpio(struct gpio_desc *gpio, bool *driven,
					 unsigned int pulse_min_us,
					 unsigned int pulse_max_us,
					 unsigned int settle_ms)
{
	int ret;

	if (!gpio)
		return -ENODEV;
	ret = gpiod_direction_output(gpio, 0);
	if (ret)
		return ret;
	*driven = true;
	gpiod_set_value_cansleep(gpio, 1);
	usleep_range(pulse_min_us, pulse_max_us);
	gpiod_set_value_cansleep(gpio, 0);
	ret = gpiod_direction_input(gpio);
	if (ret)
		return ret;
	*driven = false;
	if (settle_ms)
		msleep(settle_ms);
	return 0;
}

#endif /* ESP_SPI_COMMON_H */
