// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 */
#ifndef _ESP_DECL_H_
#define _ESP_DECL_H_

#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include "esp.h"

/* Interrupt Status */
#define ESP_SLAVE_BIT0_INT             BIT(0)
#define ESP_SLAVE_BIT1_INT             BIT(1)
#define ESP_SLAVE_BIT2_INT             BIT(2)
#define ESP_SLAVE_BIT3_INT             BIT(3)
#define ESP_SLAVE_BIT4_INT             BIT(4)
#define ESP_SLAVE_BIT5_INT             BIT(5)
#define ESP_SLAVE_BIT6_INT             BIT(6)
#define ESP_SLAVE_BIT7_INT             BIT(7)
#define ESP_SLAVE_RX_UNDERFLOW_INT     BIT(16)
#define ESP_SLAVE_TX_OVERFLOW_INT      BIT(17)
#define ESP_SLAVE_RX_NEW_PACKET_INT    BIT(23)


#define ESP_SLAVE_CMD53_END_ADDR       0x1F800
#define ESP_SLAVE_LEN_MASK             0xFFFFF
#define ESP_BLOCK_SIZE                 512
#define ESP_RX_BYTE_MAX                0x100000
#define ESP_RX_BUFFER_SIZE             15872
#define ESP_HOST_TX_AGGR_SIZE          ESP_RX_BUFFER_SIZE
#define ESP_HOST_RX_AGGR_SIZE          15872
#define ESP_HOST_TX_LATENCY_BYPASS_SIZE 256

#define ESP_TX_BUFFER_MASK             0xFFF
#define ESP_TX_BUFFER_MAX              0x1000
#define ESP_MAX_BUF_CNT                10

#define ESP_SLAVE_SLCHOST_BASE         0x3FF55000

#define ESP_SLAVE_SCRATCH_REG_7        (ESP_SLAVE_SLCHOST_BASE + 0x8C)
/* SLAVE registers */
/* Interrupt Registers */
#define ESP_SLAVE_INT_RAW_REG          (ESP_SLAVE_SLCHOST_BASE + 0x50)
#define ESP_SLAVE_INT_ST_REG           (ESP_SLAVE_SLCHOST_BASE + 0x58)
#define ESP_SLAVE_INT_CLR_REG          (ESP_SLAVE_SLCHOST_BASE + 0xD4)
#define ESP_SLAVE_INT_ENA_REG          (ESP_SLAVE_SLCHOST_BASE + 0xEC)

/* Data path registers*/
#define ESP_SLAVE_PACKET_LEN_REG       (ESP_SLAVE_SLCHOST_BASE + 0x60)
#define ESP_SLAVE_TOKEN_RDATA          (ESP_SLAVE_SLCHOST_BASE + 0x44)

/* Scratch registers*/
#define ESP_SLAVE_SCRATCH_REG_0        (ESP_SLAVE_SLCHOST_BASE + 0x6C)
#define ESP_SDIO_RESET_GEN_REG         ESP_SLAVE_SCRATCH_REG_0
#define ESP_SDIO_RESET_DONE_REG        (ESP_SLAVE_SCRATCH_REG_0 + 1)
#define ESP_SLAVE_SCRATCH_REG_1        (ESP_SLAVE_SLCHOST_BASE + 0x70)
#define ESP_SLAVE_SCRATCH_REG_2        (ESP_SLAVE_SLCHOST_BASE + 0x74)
#define ESP_SLAVE_SCRATCH_REG_3        (ESP_SLAVE_SLCHOST_BASE + 0x78)
#define ESP_SLAVE_SCRATCH_REG_4        (ESP_SLAVE_SLCHOST_BASE + 0x7C)
#define ESP_SLAVE_SCRATCH_REG_6        (ESP_SLAVE_SLCHOST_BASE + 0x88)
#define ESP_SLAVE_SCRATCH_REG_8        (ESP_SLAVE_SLCHOST_BASE + 0x9C)
#define ESP_SLAVE_SCRATCH_REG_9        (ESP_SLAVE_SLCHOST_BASE + 0xA0)
#define ESP_SLAVE_SCRATCH_REG_10       (ESP_SLAVE_SLCHOST_BASE + 0xA4)
#define ESP_SLAVE_SCRATCH_REG_11       (ESP_SLAVE_SLCHOST_BASE + 0xA8)
#define ESP_SLAVE_SCRATCH_REG_12       (ESP_SLAVE_SLCHOST_BASE + 0xAC)
#define ESP_SLAVE_SCRATCH_REG_13       (ESP_SLAVE_SLCHOST_BASE + 0xB0)
#define ESP_SLAVE_SCRATCH_REG_14       (ESP_SLAVE_SLCHOST_BASE + 0xB4)
#define ESP_SLAVE_SCRATCH_REG_15       (ESP_SLAVE_SLCHOST_BASE + 0xB8)

#define ESP_ADDRESS_MASK              0x3FF

#define ESP_VENDOR_ID_1             0x6666
#define ESP_DEVICE_ID_ESP32_1       0x2222
#define ESP_DEVICE_ID_ESP32_2       0x3333

#define ESP_VENDOR_ID_2             0x0092
#define ESP_DEVICE_ID_C5_C6_C61_1   0x6666
#define ESP_DEVICE_ID_C5_C6_C61_2   0x7777

struct esp_sdio_context {
	struct esp_adapter     *adapter;
	struct sdio_func       *func;
	struct sk_buff_head    tx_q[MAX_PRIORITY_QUEUES];
	struct sk_buff_head    rx_q;
	u32                    rx_byte_count;
	u32                    tx_buffer_count;
	/* Bumped at each incarnation/recovery boundary. TX captures this before
	 * CMD53 and rechecks after claiming the MMC host so an old aggregate
	 * cannot commit after counters are rebased. */
	atomic_t               tx_epoch;
	bool                   irq_claimed;
	u32			sdio_clk_mhz;
	/* TX kthread wakeup: enqueuing a skb wakes tx_process instead of
	 * relying on its 10-20ms usleep poll. Driven by wake_up()/wait_event. */
	wait_queue_head_t      tx_waitq;
	atomic_t               tx_aggr_has_hci;
	wait_queue_head_t      tx_aggr_waitq;
	/* DMA-safe SDIO buffers allocated once at probe (not per IRQ/packet):
	 * reg_buf = ISR INT_ST / INT_CLR (1 word);
	 * rx_len_buf = PACKET_LEN only (1 word);
	 * token_buf = TOKEN_RDATA only (1 word);
	 * tx_aggr_buf = Host→ESP CMD53 aggregate (ESP_TX_AGGR_SIZE_MAX).
	 * bcm2835-mmc DMA-maps CMD53 sg lists. GFP_DMA32 is the 32-bit DMA zone on
	 * 64-bit hosts; GFP_DMA is ZONE_DMA (needed on 32-bit Raspberry Pi). Do not
	 * use GFP_DMA on x86_64 — that is the ISA 16MB zone and the 15872-byte
	 * aggregate can fail probe. */
	u32                    *reg_buf;
	u32                    *rx_len_buf;
	u32                    *token_buf;
	u8                     *tx_aggr_buf;
	atomic_t               rx_pending;
	/* Re-arm NEW_PACKET when PACKET_LEN has not settled after ISR ACK. */
	struct delayed_work    rx_len_retry_work;
	u8                     rx_len_retry_count;
};

#endif
