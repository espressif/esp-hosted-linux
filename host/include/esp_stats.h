// SPDX-License-Identifier: GPL-2.0-only
/*
 * Espressif Systems Wireless LAN device driver
 *
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 */
#ifndef __ESP_STAT__H__
#define __ESP_STAT__H__

#include "esp.h"

#define TEST_RAW_TP 1

extern u32 raw_tp_mode;

#if TEST_RAW_TP

#define TEST_RAW_TP__BUF_SIZE    1460

#define ESP_TEST_RAW_TP__RX      0
#define ESP_TEST_RAW_TP__TX      1

void esp_raw_tp_queue_resume(void);
void esp_raw_tp_queue_pause(void);
u32 esp_raw_tp_tx_seq_get(void);
void esp_raw_tp_set_seq(struct esp_payload_header *header, u32 seq);
void esp_raw_tp_tx_complete(u32 run_id, u32 frame_count);
void esp_raw_tp_tx_failed(u32 frame_count);
u32 esp_raw_tp_alloc_run_id(void);
u32 esp_raw_tp_get_run_id(void);
#endif

void test_raw_tp_cleanup(void);
void update_test_raw_tp_rx_stats(const struct esp_payload_header *header,
				 u16 len);

#endif
