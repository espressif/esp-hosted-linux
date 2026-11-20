/* Actual ESP-Hosted standard SPI command/stateful/dummy functions inserted
 * from host/spi/esp_spi.c. No I/O, GPIO, controller or DMA simulation here.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "adapter.h"
#include "esp_host_wire_bounds.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t __le16;
typedef uint32_t __le32;
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error This native SPI parser harness requires little-endian host
#endif
#define esp_wire_le16_to_cpu(v) (v)
#define esp_wire_cpu_to_le16(v) (v)
#define le16_to_cpu(v) (v)
#define le32_to_cpu(v) (v)
#define SPI_BUF_SIZE 1600
#define ESP_FIRMWARE_CHIP_UNRECOGNIZED 0xff
#define ESP_FIRMWARE_CHIP_ESP32C6 0x0D
#define IRAM_ATTR

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 0x102
#define GPIO_NUM_MAX 31
#define GPIO_IS_VALID_OUTPUT_GPIO(g) (((1ULL << (g)) & 0x7ffffffULL) != 0)
#define GPIO_MOSI 7
#define GPIO_MISO 2
#define GPIO_SCLK 6
#define GPIO_CS   10
static const char TAG[] = "HARNESS";

static inline void esp_log_noop(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
}
#define ESP_LOGE esp_log_noop

static inline void esp_err(const char *fmt, ...)
{
    (void)fmt;
}

static inline void esp_verbose(const char *fmt, ...)
{
    (void)fmt;
}

struct esp_adapter;

struct sk_buff {
    u8 *data;
    size_t len;
};

struct sk_buff_head {
    struct sk_buff *last;
    unsigned int count;
};

static struct {
    struct esp_adapter *adapter;
    struct sk_buff_head rx_q[MAX_PRIORITY_QUEUES];
} spi_context;

static u8 data_path = 1;
static unsigned int g_boot_rescue_retired;
static unsigned int g_rcp_fail_count;
static unsigned int g_new_pkt_intr_count;

static void esp_spi_retire_boot_rescue(void)
{
    g_boot_rescue_retired++;
}

static void esp_spi_fail_rcp_rx(const char *reason)
{
    (void)reason;
    g_rcp_fail_count++;
}

static void esp_process_new_packet_intr(struct esp_adapter *adapter)
{
    (void)adapter;
    g_new_pkt_intr_count++;
}

static void skb_trim(struct sk_buff *skb, size_t len)
{
    if (skb && len < skb->len)
        skb->len = len;
}

static void skb_queue_tail(struct sk_buff_head *q, struct sk_buff *skb)
{
    q->last = skb;
    q->count++;
}

typedef struct {
    void *user;
} spi_slave_transaction_t;

typedef struct {
    bool has_tx;
} spi_slot_t;

static const u8 gpio_handshake = 2;
static const u8 gpio_data_ready = 3;
static unsigned int spi_tx_queue[MAX_PRIORITY_QUEUES];
static bool g_handshake_level;
static bool g_data_ready_level;
static void (*g_after_data_ready_low_hook)(void);

static unsigned int uxQueueMessagesWaiting(unsigned int q_count)
{
    return q_count;
}

static void spi_gpio_set_high(u8 gpio)
{
    if (gpio == gpio_handshake)
        g_handshake_level = true;
    else if (gpio == gpio_data_ready)
        g_data_ready_level = true;
}

static void spi_gpio_set_low(u8 gpio)
{
    if (gpio == gpio_handshake) {
        g_handshake_level = false;
    } else if (gpio == gpio_data_ready) {
        if (g_after_data_ready_low_hook)
            g_after_data_ready_low_hook();
        g_data_ready_level = false;
    }
}

/* GENERATED_SPI_FUNCTIONS */

static void test_command_parser(void)
{
    u8 data[128] = {0};
    struct esp_payload_header *h = (struct esp_payload_header *)data;
    struct command_header *cmd = (struct command_header *)(data + sizeof(*h));
    struct sk_buff skb = {.data = data, .len = sizeof(*h) + sizeof(*cmd)};
    u8 opcode = 0;
    u16 seq = 0;
    h->packet_type = PACKET_TYPE_COMMAND_REQUEST;
    h->offset = sizeof(*h);
    cmd->cmd_code = CMD_SCAN_REQUEST;
    cmd->seq_num = 0x1234;
    assert(spi_get_cmd_info(&skb, &opcode, &seq));
    assert(opcode == CMD_SCAN_REQUEST && seq == 0x1234);
    assert(spi_get_cmd_info(&skb, NULL, NULL));
    skb.len = sizeof(*h) - 1;
    assert(!spi_get_cmd_info(&skb, &opcode, &seq));
    skb.len = sizeof(*h) + sizeof(*cmd) - 1;
    assert(!spi_get_cmd_info(&skb, &opcode, &seq));
    skb.len = sizeof(*h) + sizeof(*cmd);
    h->packet_type = PACKET_TYPE_DATA;
    assert(!spi_get_cmd_info(&skb, &opcode, &seq));
    h->packet_type = PACKET_TYPE_COMMAND_REQUEST;
    h->offset = 65535;
    assert(!spi_get_cmd_info(&skb, &opcode, &seq));
    assert(!spi_get_cmd_info(NULL, &opcode, &seq));
}

static void test_stateful_classifier(void)
{
    u8 data[32] = {0};
    struct esp_payload_header *h = (struct esp_payload_header *)data;
    struct sk_buff skb = {.data = data, .len = sizeof(*h)};
    h->if_type = ESP_HCI_IF;
    assert(spi_is_stateful_packet(&skb));
    h->if_type = ESP_RCP_IF;
    assert(spi_is_stateful_packet(&skb));
    h->if_type = ESP_STA_IF;
    assert(!spi_is_stateful_packet(&skb));
    h->if_type = ESP_AP_IF;
    assert(!spi_is_stateful_packet(&skb));
    skb.len--;
    h->if_type = ESP_HCI_IF;
    assert(!spi_is_stateful_packet(&skb));
    assert(!spi_is_stateful_packet(NULL));
}

static void test_exact_dummy_rx_sentinel(void)
{
    struct esp_payload_header h = {0};
    assert(!spi_is_dummy_rx(NULL));
    h.if_type = 0xf;
    h.if_num = 0xf;
    assert(spi_is_dummy_rx(&h));
    h.if_type = ESP_STA_IF; assert(!spi_is_dummy_rx(&h)); h.if_type = 0xf;
    h.if_num = 0; assert(!spi_is_dummy_rx(&h)); h.if_num = 0xf;
    h.flags = 1; assert(!spi_is_dummy_rx(&h)); h.flags = 0;
    h.packet_type = 1; assert(!spi_is_dummy_rx(&h)); h.packet_type = 0;
    h.reserved1 = 1; assert(!spi_is_dummy_rx(&h)); h.reserved1 = 0;
    h.len = 1; assert(!spi_is_dummy_rx(&h)); h.len = 0;
    h.offset = 1; assert(!spi_is_dummy_rx(&h)); h.offset = 0;
    h.checksum = 1; assert(!spi_is_dummy_rx(&h)); h.checksum = 0;
    h.reserved2 = 1; assert(!spi_is_dummy_rx(&h)); h.reserved2 = 0;
    h.reserved3 = 1; assert(!spi_is_dummy_rx(&h)); h.reserved3 = 0;
    assert(spi_is_dummy_rx(&h));
}

static size_t build_bootup_frame(u8 *frame, u8 packet_type, u8 cap_len, u32 cap_val)
{
    struct esp_payload_header *hdr = (struct esp_payload_header *)frame;
    struct esp_internal_bootup_event *evt =
        (struct esp_internal_bootup_event *)(frame + sizeof(*hdr));
    u8 *pos = evt->data;
    u16 tlv_len = 0;

    memset(frame, 0, SPI_BUF_SIZE);
    hdr->if_type = ESP_INTERNAL_IF;
    hdr->packet_type = packet_type;
    hdr->offset = sizeof(*hdr);

    evt->header.event_code = ESP_INTERNAL_BOOTUP_EVENT;
    evt->header.status = 0;

    /* Board chip ID TLV */
    *pos++ = ESP_BOOTUP_FIRMWARE_CHIP_ID;
    *pos++ = 1;
    *pos++ = ESP_FIRMWARE_CHIP_ESP32C6;
    tlv_len += 3;

    /* Capability TLV (1, 2, or 4 bytes) */
    *pos++ = ESP_BOOTUP_CAPABILITY;
    *pos++ = cap_len;
    memcpy(pos, &cap_val, cap_len);
    pos += cap_len;
    tlv_len += 2 + cap_len;

    /* FW data TLV */
    *pos++ = ESP_BOOTUP_FW_DATA;
    *pos++ = sizeof(struct fw_data);
    memset(pos, 0, sizeof(struct fw_data));
    pos += sizeof(struct fw_data);
    tlv_len += 2 + sizeof(struct fw_data);

    evt->len = (u8)tlv_len;
    evt->header.len = tlv_len + 1;
    hdr->len = (u16)(offsetof(struct esp_internal_bootup_event, data) + tlv_len);
    hdr->checksum = 0;
    if (cap_val & ESP_CHECKSUM_ENABLED)
        hdr->checksum = compute_checksum(frame, sizeof(*hdr) + hdr->len);
    return sizeof(*hdr) + hdr->len;
}

static void test_bootstrap_frame_valid_contracts(void)
{
    u8 frame[SPI_BUF_SIZE];
    struct esp_payload_header *hdr = (struct esp_payload_header *)frame;
    u16 payload_len = 0;
    const u32 cap_csum = ESP_WLAN_SPI_SUPPORT | ESP_CHECKSUM_ENABLED;
    const u32 cap_no_csum = ESP_WLAN_SPI_SUPPORT;

    /* Current PACKET_TYPE_EVENT and legacy PACKET_TYPE_DATA boot events */
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_csum);
    assert(esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));
    assert(payload_len > 0);

    build_bootup_frame(frame, PACKET_TYPE_DATA, 4, cap_csum);
    assert(esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));

    /* 1-byte and 2-byte capability TLV encodings */
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 1, cap_csum & 0xffU);
    assert(esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));

    build_bootup_frame(frame, PACKET_TYPE_EVENT, 2, cap_csum & 0xffffU);
    assert(esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));

    /* Invalid capability length (3 bytes) must fail */
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 3, cap_csum);
    assert(!esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));

    /* First-bit MISO corruption (0x03 -> 0x83, flipping if_num from 0 to 8)
     * must fail both when SPI checksum is enabled and when it is disabled. */
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_csum);
    frame[0] ^= 0x80U;
    assert(hdr->if_type == ESP_INTERNAL_IF && hdr->if_num == 8);
    assert(!esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));

    /* Clean checksum-disabled boot frame (CONFIG_ESP_SPI_CHECKSUM=n) */
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_no_csum);
    assert(hdr->checksum == 0);
    assert(esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));

    /* Same checksum-disabled boot frame with byte0 bit7 flipped (0x03 -> 0x83) */
    frame[0] ^= 0x80U;
    assert(hdr->if_type == ESP_INTERNAL_IF && hdr->if_num == 8);
    assert(!esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));

    /* Non-canonical header flags or reserved bytes must also fail */
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_no_csum);
    hdr->flags = MORE_FRAGMENT;
    assert(!esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));

    /* Checksum-disabled BOOT with outer hdr->len inflated or shrunk must fail */
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_no_csum);
    hdr->len += 1;
    assert(!esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));
    hdr->len -= 2;
    assert(!esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));

    /* Checksum-disabled BOOT with evt->header.len mismatching evt->len + 1 must fail */
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_no_csum);
    ((struct esp_internal_bootup_event *)(frame + sizeof(*hdr)))->header.len += 1;
    assert(!esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));
    ((struct esp_internal_bootup_event *)(frame + sizeof(*hdr)))->header.len -= 2;
    assert(!esp_spi_bootstrap_frame_valid(NULL, frame, true, &payload_len));
}

static void reset_rx_harness_state(void)
{
    memset(&spi_context, 0, sizeof(spi_context));
    data_path = 1;
    g_boot_rescue_retired = 0;
    g_rcp_fail_count = 0;
    g_new_pkt_intr_count = 0;
}

static void test_process_rx_buf_boot_and_internal_validation(void)
{
    u8 frame[SPI_BUF_SIZE];
    struct esp_payload_header *hdr = (struct esp_payload_header *)frame;
    struct sk_buff skb;
    const u32 cap_no_csum = ESP_WLAN_SPI_SUPPORT;
    const u32 cap_csum = ESP_WLAN_SPI_SUPPORT | ESP_CHECKSUM_ENABLED;
    size_t frame_len;

    /* 1. Clean checksum-disabled BOOT frame on normal RX path must succeed */
    reset_rx_harness_state();
    frame_len = build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_no_csum);
    skb.data = frame;
    skb.len = SPI_BUF_SIZE;
    assert(process_rx_buf(&skb, true) == 0);
    assert(skb.len == frame_len);
    assert(g_boot_rescue_retired == 1);
    assert(g_rcp_fail_count == 0);
    assert(spi_context.rx_q[PRIO_Q_HIGH].count == 1);
    assert(g_new_pkt_intr_count == 1);

    /* 2. Checksum-disabled BOOT with byte0 bit7 flipped (0x03 -> 0x83,
     *    if_type == ESP_INTERNAL_IF, if_num == 8) must be dropped BEFORE
     *    queueing on rx_q[PRIO_Q_HIGH] and must not retire boot rescue. */
    reset_rx_harness_state();
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_no_csum);
    frame[0] ^= 0x80U;
    assert(hdr->if_type == ESP_INTERNAL_IF && hdr->if_num == 8);
    skb.data = frame;
    skb.len = SPI_BUF_SIZE;
    assert(process_rx_buf(&skb, true) == -EINVAL);
    assert(g_boot_rescue_retired == 0);
    assert(g_rcp_fail_count == 1);
    assert(spi_context.rx_q[PRIO_Q_HIGH].count == 0);
    assert(g_new_pkt_intr_count == 0);

    /* 3. Checksum-disabled BOOT with corrupted packet_type (while keeping
     *    ESP_INTERNAL_BOOTUP_EVENT) must also be dropped before queueing. */
    reset_rx_harness_state();
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_no_csum);
    hdr->packet_type = PACKET_TYPE_COMMAND_REQUEST;
    skb.data = frame;
    skb.len = SPI_BUF_SIZE;
    assert(process_rx_buf(&skb, true) == -EINVAL);
    assert(g_boot_rescue_retired == 0);
    assert(g_rcp_fail_count == 1);
    assert(spi_context.rx_q[PRIO_Q_HIGH].count == 0);

    /* 3b. Checksum-disabled BOOT with outer hdr->len mismatch on normal RX path */
    reset_rx_harness_state();
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_no_csum);
    hdr->len += 1;
    skb.data = frame;
    skb.len = SPI_BUF_SIZE;
    assert(process_rx_buf(&skb, true) == -EINVAL);
    assert(g_boot_rescue_retired == 0);
    assert(spi_context.rx_q[PRIO_Q_HIGH].count == 0);

    /* 3c. Checksum-disabled BOOT with evt->header.len mismatch on normal RX path */
    reset_rx_harness_state();
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_no_csum);
    ((struct esp_internal_bootup_event *)(frame + sizeof(*hdr)))->header.len += 1;
    skb.data = frame;
    skb.len = SPI_BUF_SIZE;
    assert(process_rx_buf(&skb, true) == -EINVAL);
    assert(g_boot_rescue_retired == 0);
    assert(spi_context.rx_q[PRIO_Q_HIGH].count == 0);

    /* 4. Checksum-enabled BOOT frame with bad checksum must be dropped */
    reset_rx_harness_state();
    build_bootup_frame(frame, PACKET_TYPE_EVENT, 4, cap_csum);
    frame[sizeof(*hdr)] ^= 0x01U;
    skb.data = frame;
    skb.len = SPI_BUF_SIZE;
    assert(process_rx_buf(&skb, true) == -EINVAL);
    assert(g_boot_rescue_retired == 0);
    assert(spi_context.rx_q[PRIO_Q_HIGH].count == 0);

    /* 5. Non-canonical zero-length frame (not 0xF/0xF dummy) must be dropped */
    reset_rx_harness_state();
    memset(frame, 0, sizeof(frame));
    hdr->if_type = ESP_STA_IF;
    hdr->offset = sizeof(*hdr);
    hdr->len = 0;
    skb.data = frame;
    skb.len = SPI_BUF_SIZE;
    assert(process_rx_buf(&skb, true) == -EINVAL);
    assert(g_rcp_fail_count == 1);
    assert(spi_context.rx_q[PRIO_Q_LOW].count == 0);

    /* 6. Canonical 0xF/0xF dummy frame returns -ENODATA without RCP failure */
    reset_rx_harness_state();
    memset(frame, 0, sizeof(frame));
    hdr->if_type = 0xf;
    hdr->if_num = 0xf;
    skb.data = frame;
    skb.len = SPI_BUF_SIZE;
    assert(process_rx_buf(&skb, true) == -ENODATA);
    assert(g_rcp_fail_count == 0);
}

static void test_gpio_validation(void)
{
    assert(spi_validate_output_gpio(3, "handshake") == ESP_OK);
    assert(spi_validate_output_gpio(15, "data_ready") == ESP_OK);
    assert(spi_validate_output_gpio(-1, "handshake") == ESP_ERR_INVALID_ARG);
    assert(spi_validate_output_gpio(29, "data_ready") == ESP_ERR_INVALID_ARG);
    assert(spi_validate_output_gpio(32, "handshake") == ESP_ERR_INVALID_ARG);
    assert(spi_validate_output_gpio(255, "data_ready") == ESP_ERR_INVALID_ARG);
    assert(spi_validate_output_gpio(GPIO_MOSI, "handshake") == ESP_ERR_INVALID_ARG);
    assert(spi_validate_output_gpio(GPIO_MISO, "handshake") == ESP_ERR_INVALID_ARG);
    assert(spi_validate_output_gpio(GPIO_SCLK, "data_ready") == ESP_ERR_INVALID_ARG);
    assert(spi_validate_output_gpio(GPIO_CS, "data_ready") == ESP_ERR_INVALID_ARG);
}

static void simulate_concurrent_e2h_producer(void)
{
    /* Simulate esp_spi_write() enqueueing an E2H packet and asserting
     * DATA_READY right before queue_next_transaction()'s GPIO clear takes
     * effect. */
    spi_tx_queue[PRIO_Q_LOW] = 1;
    spi_gpio_set_high(gpio_data_ready);
}

/*
 * Regression for the two-stage SPI E2H readiness contract and the
 * queue_next_transaction() vs esp_spi_write() DATA_READY race, executing the
 * actual extracted firmware functions spi_sync_dummy_slot_data_ready() and
 * spi_post_setup_cb().
 */
static void test_fresh_handshake_generation_gate(void)
{
    /* A level still high from the slot just consumed is stale and must not
     * trigger a second transfer before firmware arms the next DMA slot. */
    assert(!esp_spi_next_slot_ready(false, 7, 8));
    assert(!esp_spi_next_slot_ready(true, 7, 7));

    /* A fresh rising edge proves the next slot is armed. Generation wrap is
     * also safe because the contract only needs inequality. */
    assert(esp_spi_next_slot_ready(true, 7, 8));
    assert(esp_spi_next_slot_ready(true, UINT32_MAX, 0));
}

static void test_two_stage_dummy_to_e2h_slot_sequence(void)
{
    u8 slot_wire[2][SPI_BUF_SIZE];
    struct esp_payload_header *slot0_hdr = (struct esp_payload_header *)slot_wire[0];
    struct esp_payload_header *slot1_hdr = (struct esp_payload_header *)slot_wire[1];
    spi_slot_t slot0 = {.has_tx = false};
    spi_slot_t slot1 = {.has_tx = true};
    spi_slave_transaction_t trans0 = {.user = &slot0};
    spi_slave_transaction_t trans1 = {.user = &slot1};
    u16 payload_len = 0;

    memset(spi_tx_queue, 0, sizeof(spi_tx_queue));
    g_handshake_level = false;
    g_data_ready_level = true;
    g_after_data_ready_low_hook = NULL;

    /* 1. Idle slave arms slot 0 with 0xF dummy header (has_tx == false).
     *    spi_sync_dummy_slot_data_ready() clears DATA_READY; spi_post_setup_cb()
     *    raises HANDSHAKE only. */
    memset(slot_wire[0], 0, SPI_BUF_SIZE);
    slot0_hdr->if_type = 0xf;
    slot0_hdr->if_num = 0xf;
    assert(!spi_sync_dummy_slot_data_ready());
    spi_post_setup_cb(&trans0);
    assert(g_handshake_level && !g_data_ready_level);

    /* 2. Verify race-safety: a concurrent esp_spi_write() producer enqueues a
     *    frame and sets DATA_READY while queue_next_transaction() is clearing
     *    DATA_READY for a dummy slot. The recheck after clear must re-assert
     *    DATA_READY so the wakeup behind the armed dummy is never lost. */
    g_after_data_ready_low_hook = simulate_concurrent_e2h_producer;
    assert(spi_sync_dummy_slot_data_ready());
    g_after_data_ready_low_hook = NULL;
    assert(g_handshake_level && g_data_ready_level);

    /* 3. Host observes (HANDSHAKE && DATA_READY), clocks slot 0, and discards
     *    the 0xF dummy sentinel. */
    assert(spi_is_dummy_rx(slot0_hdr));
    g_handshake_level = false;

    /* 4. Completion of slot 0 advances queue_next_transaction(), which dequeues
     *    the pending E2H frame into slot 1 (has_tx == true). spi_post_setup_cb()
     *    asserts both DATA_READY and HANDSHAKE once slot 1 DMA is armed. */
    spi_tx_queue[PRIO_Q_LOW] = 0;
    g_data_ready_level = false;
    build_bootup_frame(slot_wire[1], PACKET_TYPE_EVENT, 4,
                       ESP_WLAN_SPI_SUPPORT | ESP_CHECKSUM_ENABLED);
    spi_post_setup_cb(&trans1);
    assert(g_handshake_level && g_data_ready_level);

    /* 5. Host clocks slot 1: not a dummy frame, valid E2H payload. */
    assert(!spi_is_dummy_rx(slot1_hdr));
    assert(esp_spi_bootstrap_frame_valid(NULL, slot_wire[1], true, &payload_len));
    assert(payload_len == slot1_hdr->len);
}

int main(void)
{
    test_command_parser();
    test_stateful_classifier();
    test_exact_dummy_rx_sentinel();
    test_bootstrap_frame_valid_contracts();
    test_process_rx_buf_boot_and_internal_validation();
    test_gpio_validation();
    test_fresh_handshake_generation_gate();
    test_two_stage_dummy_to_e2h_slot_sequence();
    puts("PASS: production SPI command parser, HCI/RCP stateful, dummy RX sentinel, RX boot gate, GPIO validation, fresh-handshake requeue gate, and two-stage readiness");
    return 0;
}
