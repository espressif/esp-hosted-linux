/* Actual esp_spi_hd.c pure ACK/batching functions injected by generator.
 * Only logical fastpath/ACK gates; no DMA, QIO bus, IRQ or recovery tested.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "adapter.h"
#include "esp_host_wire_bounds.h"
#include "esp_spi_hd_wire.h"
typedef uint8_t u8;
typedef uint16_t u16;
#define esp_wire_le16_to_cpu(v) (v)
#define SPI_HD_BUF_SIZE 1600U
typedef uint32_t u32;
#define READ_ONCE(v) (v)
#define SPI_HD_TX_SEGMENT_MIN 4U
#define SPI_TX_QUAD (1U << 9)
#define SPI_RX_QUAD (1U << 10)
#define SPI_NBITS_SINGLE 1U
#define SPI_NBITS_QUAD 4U

#define SPIHD_SIO_HDR_MAX 3U
#define SPIHD_BULK_MIN_BYTES 256U
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
static struct { bool append_dma; void *spi; } spi_hd;
static u32 spihd_rx_limit = 1600;
static size_t transfer_limit = 3200, message_limit = 3209;
static size_t spi_max_transfer_size(void *unused) { (void)unused; return transfer_limit; }
static size_t spi_max_message_size(void *unused) { (void)unused; return message_limit; }

/* GENERATED_SPIDH_FUNCTIONS */

static void reset_config(void)
{
    spi_hd.append_dma = false;
    spihd_rx_limit = 1600;
    transfer_limit = 3200;
    message_limit = 3209;
}

static void test_wrdma_batch_gating(void)
{
    reset_config();
    assert(spihd_combined_wrdma(260, 4));
    assert(!spihd_combined_wrdma(256, 4));
    assert(!spihd_combined_wrdma(258, 4));
    assert(!spihd_combined_wrdma(260, 1));
    message_limit = 265;
    assert(!spihd_combined_wrdma(260, 4));
    message_limit = 266;
    assert(spihd_combined_wrdma(260, 4));
    transfer_limit = 259;
    assert(!spihd_combined_wrdma(260, 4));
}

static void test_phase_lane_capability_guards(void)
{
    u32 tx = SPI_TX_QUAD;
    u32 rx = SPI_RX_QUAD;
    u32 both = tx | rx;

    assert(spihd_phase_lane_supported(0, 1, 1));
    assert(spihd_phase_lane_supported(0, 0, 1));
    assert(spihd_phase_lane_supported(0, 1, 0));
    assert(!spihd_phase_lane_supported(0, 4, 0));
    assert(!spihd_phase_lane_supported(0, 0, 4));

    assert(spihd_phase_lane_supported(tx, 4, 0));
    assert(!spihd_phase_lane_supported(tx, 0, 4));
    assert(spihd_phase_lane_supported(rx, 0, 4));
    assert(!spihd_phase_lane_supported(rx, 4, 0));
    assert(spihd_phase_lane_supported(both, 4, 4));
    assert(!spihd_phase_lane_supported(both, 2, 1));
    assert(!spihd_phase_lane_supported(both, 1, 2));
}

static void test_directional_quad_negotiation(void)
{
    u32 tx = SPI_TX_QUAD;
    u32 rx = SPI_RX_QUAD;
    u32 both = tx | rx;
    u32 none = 0;

    assert(spihd_select_data_lines(both, true, 4) == 4);
    assert(spihd_select_data_lines(both, false, 4) == 4);
    assert(spihd_select_data_lines(tx, true, 4) == 4);
    assert(spihd_select_data_lines(tx, false, 4) == 1);
    assert(spihd_select_data_lines(rx, true, 4) == 1);
    /* E2H needs quad TX for its address, so RX-only falls back to SIO. */
    assert(spihd_select_data_lines(rx, false, 4) == 1);
    assert(spihd_select_data_lines(none, true, 4) == 1);
    assert(spihd_select_data_lines(none, false, 4) == 1);
    /* Before the ESP capability handshake, all traffic remains SIO. */
    assert(spihd_select_data_lines(both, true, 1) == 1);
    assert(spihd_select_data_lines(both, false, 1) == 1);
}

static void test_auto_segment_limits_follow_active_slave(void)
{
    assert(spihd_effective_tx_segment(3200, 3200, 3203) == 3200);
    assert(spihd_effective_tx_segment(1600, 3200, 3203) == 1600);
    assert(spihd_effective_tx_segment(3200, 1600, 3203) == 1600);
    assert(spihd_effective_tx_segment(3200, 3200, 1603) == 1600);
    assert(spihd_effective_tx_segment(3200, 3200, 2) == 0);
    assert(spihd_effective_tx_segment(3200, 3200, 6) == 0);
}

static void test_ack_required_fail_closed(void)
{
    reset_config();
    assert(spihd_rx_ack_required(0, 1));
    spi_hd.append_dma = true;
    assert(!spihd_rx_ack_required(0, 1));
    assert(!spihd_rx_ack_required(0, spihd_rx_limit));
    assert(spihd_rx_ack_required(0, 0));
    assert(spihd_rx_ack_required(0, spihd_rx_limit + 1));
    assert(spihd_rx_ack_required(1, 1));
    spi_hd.append_dma = false;
    assert(spihd_rx_ack_required(0, 1));
}

static void test_aligned_frame_spans(void)
{
    u8 data[2 * SPIHD_WIRE_SPAN(sizeof(struct esp_payload_header) + 1)] = {0};
    struct esp_payload_header *first = (void *)data;
    size_t len = sizeof(*first) + 1;
    size_t span = SPIHD_WIRE_SPAN(len);
    struct esp_payload_header *second = (void *)(data + span);
    u32 frames = 0;
    unsigned int n;

    for (n = 1; n <= SPI_HD_BUF_SIZE; n++) {
        unsigned int padded = SPIHD_WIRE_SPAN(n);
        assert(padded >= n && padded - n < 4 && !(padded & 3));
    }
    first->if_type = ESP_STA_IF;
    first->offset = sizeof(*first);
    first->len = 1;
    second->if_type = ESP_STA_IF;
    second->offset = sizeof(*second);
    second->len = 1;
    assert(spihd_validate_rx_batch(data, 2 * span, &frames) == 0);
    assert(frames == 2);
    assert(spihd_validate_rx_batch(data, len, &frames) == -EPROTO);
    data[len] = 1;
    assert(spihd_validate_rx_batch(data, 2 * span, &frames) == -EPROTO);
    data[len] = 0;
    assert(spihd_validate_rx_batch(data, 2 * span - 1, &frames) == -EPROTO);
}

static void test_credit_delta(void)
{
    u32 consumed, window, delta;

    for (consumed = 0; consumed < 256; consumed++)
        for (window = 1; window <= SPIHD_MAX_RX_CREDIT_WINDOW; window++)
            for (delta = 0; delta < 256; delta++)
                assert(spihd_credit_delta(consumed + delta, consumed,
                                          window) ==
                       (delta && delta <= window ? delta : 0));
    assert(spihd_credit_delta(0, UINT32_MAX, 4) == 1);
    assert(spihd_credit_delta(0xfffffffeU, 0xffffffffU, 4) == 0);
    assert(spihd_credit_delta(0x101U, 0U, 4) == 0);
    assert(spihd_credit_delta(0U, 0x100U, 4) == 0);
    assert(spihd_credit_delta(0x100U, 0xffU, 4) == 1);
    assert(spihd_credit_delta(0x1ffU, 0xffU, 4) == 0);
    assert(spihd_credit_delta(0, 0x80000001U, 4) == 0);
    assert(spihd_credit_delta(0x80000001U, 0x80000001U, 4) == 0);
}

static void test_segment_lane_selection(void)
{
    assert(spihd_payload_lines(128, 4) == 1);
    assert(spihd_payload_lines(256, 4) == 1);
    assert(spihd_payload_lines(260, 4) == 4);
    assert(spihd_payload_lines(3200, 4) == 4);
    assert(spihd_payload_lines(3200, 1) == 1);
    assert(spihd_payload_lines(1500 % 512, 4) == 4);
    assert(spihd_payload_lines(1600 % 512, 4) == 1);
}

int main(void)
{
    test_segment_lane_selection();
    test_credit_delta();
    test_aligned_frame_spans();
    test_wrdma_batch_gating();
    test_ack_required_fail_closed();
    test_auto_segment_limits_follow_active_slave();
    test_directional_quad_negotiation();
    test_phase_lane_capability_guards();
    puts("PASS: production SPI-HD message limits, lanes and ACK gating");
    return 0;
}
