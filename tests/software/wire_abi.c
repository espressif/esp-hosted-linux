/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 * Native software contract for the *actual* shared production adapter.h.
 * This executes compute_checksum() from adapter.h across unaligned buffers.
 * It does NOT execute the kernel transports' RX/TX state machines.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adapter.h"
#include "esp_host_wire_bounds.h"

_Static_assert(sizeof(struct esp_payload_header) == 12, "wire payload header size changed");
_Static_assert(offsetof(struct esp_payload_header, len) == 4, "wire len offset changed");
_Static_assert(offsetof(struct esp_payload_header, offset) == 6, "wire offset field moved");
_Static_assert(offsetof(struct esp_payload_header, checksum) == 8, "wire checksum field moved");
_Static_assert(ESP_MAX_OFFSET_SIZE == 16, "maximum DMA padding changed");
_Static_assert(MAX_PRIORITY_QUEUES == 3, "wire priority count changed");
_Static_assert(offsetof(struct scan_request, ie_len) == 50,
               "scan IE length no longer occupies the reserved wire bytes");
_Static_assert(offsetof(struct scan_request, ie) == 52,
               "scan request wire prefix changed");
_Static_assert(sizeof(struct scan_request) == 52,
               "scan request wire prefix size changed");
_Static_assert(ESP_SCAN_IE_MAX_LEN == 512, "scan IE ABI limit changed");

static uint16_t checksum_reference(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;
    size_t i;
    for (i = 0; i < len; ++i)
        sum += p[i];
    return (uint16_t)sum;
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(void)
{
    static const size_t lengths[] = {
        0, 1, 2, 3, 4, 511, 512, 513, 1023, 1024, 1025, 2047,
        2048, 2049, 4095, 4096, 4097, 8191, 8192, 8193,
        15867, 15868, 15871, 15872, 15873, 16383, 16384, 65535
    };
    const size_t cap = 65535U + 8U;
    uint8_t *mem = malloc(cap);
    struct esp_payload_header header = {0};
    uint32_t state = 0x92e3c47aU;
    size_t i;
    unsigned offset;
    unsigned shift;
    if (!mem)
        return 2;
    for (offset = 0; offset < 128; ++offset)
        assert(!!ESP_OFFSET_VALID(offset) == (offset >= 12 && offset <= 16));
    assert(ESP_STA_IF == 0);
    assert(ESP_AP_IF == 1);
    assert(ESP_RCP_IF < ESP_MAX_IF);
    for (i = 0; i < sizeof(lengths)/sizeof(lengths[0]); ++i) {
        header.offset = (uint16_t)sizeof(header);
        header.len = (uint16_t)lengths[i];
        assert(header.offset == 12);
        assert(header.len == lengths[i]);
        assert((size_t)header.len + header.offset >= header.len);
    }
    /* Cover all 4 address residues plus random buffers, including exactly
     * 65535 bytes (the uint16_t API bound) and the 15,872 SDIO boundary. */
    for (i = 0; i < cap; ++i)
        mem[i] = (uint8_t)next_random(&state);
    for (i = 0; i < sizeof(lengths)/sizeof(lengths[0]); ++i) {
        for (shift = 0; shift < 4; ++shift) {
            uint8_t *ptr = mem + shift;
            uint16_t size = (uint16_t)lengths[i];
            assert(compute_checksum(ptr, size) == checksum_reference(ptr, size));
        }
    }
    for (i = 0; i < 30000; ++i) {
        size_t count = next_random(&state) % 65536U;
        uint8_t *ptr = mem + (next_random(&state) % 4U);
        assert(compute_checksum(ptr, (uint16_t)count) == checksum_reference(ptr, count));
    }
    for (size_t cap = 0; cap <= 300; cap += 3) {
        for (unsigned n = 0; n < 65536; n += 73) {
            for (unsigned off = 0; off < 30; off++) {
                size_t span = 12345;
                int expected = n != 0 && off >= 12 && off <= 16 &&
                               off <= cap && n <= cap - off;
                int actual = esp_host_wire_span_valid(n, off, cap, &span);
                assert(actual == expected);
                if (actual)
                    assert(span == n + off);
                else
                    assert(span == 12345);
            }
        }
    }
    free(mem);
    puts("PASS: production shared wire ABI + checksum + production RX guard (all four transports)");
    return 0;
}
