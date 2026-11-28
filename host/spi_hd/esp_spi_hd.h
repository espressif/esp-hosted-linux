// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESP-Hosted SPI half-duplex transport for Linux hosts.
 */
#ifndef _ESP_SPI_HD_H_
#define _ESP_SPI_HD_H_

#include "esp.h"

#define SPI_HD_BUF_SIZE                 1600U
#define SPI_HD_TX_BATCH_MAX             3200U
#define SPI_HD_INITIAL_CLK_MHZ          10U
#define SPI_HD_MAX_CLK_MHZ              80U
#define SPI_HD_TX_SEGMENT_MIN           4U

struct gpio_desc;
struct spi_device;

enum spihd_state {
	SPIHD_STOPPED,
	SPIHD_INITIALIZING,
	SPIHD_BOOTSTRAP,
	SPIHD_RUNNING,
	SPIHD_QUIESCING,
	SPIHD_RECOVERING,
};

struct esp_spi_hd_context {
	struct esp_adapter *adapter;
	struct spi_device *spi;
	struct sk_buff_head tx_q[MAX_PRIORITY_QUEUES];
	struct sk_buff_head rx_q[MAX_PRIORITY_QUEUES];
	struct workqueue_struct *workqueue;
	struct work_struct work;
	struct work_struct reset_work;
	bool reset_pending;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *data_ready_gpio;
	int data_ready_irq;
	u32 spi_max_hz;
	u32 control_max_hz;
	u32 payload_max_hz;
	u32 rx_byte_count;
	u32 tx_segment_count;
	u32 rx_credit_limit;   /* Slave-published cumulative queued RX credits */
	u32 tx_segment_max;
	u32 session_lo;
	u32 session_hi;
	u32 rx_credit_window;
	unsigned long boot_deadline;
	u32 logged_sclk_hz;
	void *tx_batch_alloc;
	void *sio_alloc;
	u8 *tx_batch_buf;
	u8 *sio_buf;            /* DMA-safe header+payload for coalesced SIO */
	u8 requested_clk_mhz;
	u8 spi_clk_mhz;
	u8 data_lines;
	bool append_dma;
	bool tx_len_mirror;
	enum spihd_state state;
	bool reset_gpio_driven;
	/* A WRDMA/RDDMA may have reached the wire without its WR_END/INT0. */
	bool tx_inflight;
	bool rx_inflight;
};

#endif
