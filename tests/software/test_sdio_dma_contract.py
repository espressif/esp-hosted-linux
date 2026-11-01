"""Source policy for SDIO page-backed bulk buffers.

This is a compile/source contract only. It does not emulate MMC DMA mapping.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SDIODMABufferContractTests(unittest.TestCase):
    def test_persistent_cmd53_buffers_use_page_allocator(self):
        source = (ROOT / "host/sdio/esp_sdio.c").read_text()

        self.assertIn("#define ESP_SDIO_BULK_GFP GFP_KERNEL", source)

        expected = {
            "tx_aggr_buf": (
                r"context->tx_aggr_buf\s*=\s*\(u8 \*\)__get_free_pages\(\s*"
                r"ESP_SDIO_BULK_GFP \| __GFP_ZERO,\s*"
                r"get_order\(ESP_TX_AGGR_SIZE_MAX\)\)"
            ),
            "rx_bounce_buf": (
                r"context->rx_bounce_buf\s*=\s*\(u8 \*\)__get_free_pages\(\s*"
                r"ESP_SDIO_BULK_GFP,\s*get_order\(ESP_HOST_RX_AGGR_SIZE\)\)"
            ),
        }
        for name, pattern in expected.items():
            with self.subTest(buffer=name):
                self.assertRegex(source, pattern)

        # Bulk buffers exist before IRQ registration. The ISR must not depend
        # on an allocation that can fail after the function IRQ is live.
        alloc = source.index("context->tx_aggr_buf = (u8 *)__get_free_pages")
        claim_irq = source.index("sdio_claim_irq(func, esp_handle_isr)", alloc)
        self.assertLess(alloc, claim_irq)

    def test_page_buffers_are_freed_together_and_pointers_cleared(self):
        source = (ROOT / "host/sdio/esp_sdio.c").read_text()
        fn = source.split("static void esp_sdio_free_dma_bufs", 1)[1].split(
            "#ifdef ESP_DEBUG_STATS", 1)[0]
        expected = {
            "tx_aggr_buf": "ESP_TX_AGGR_SIZE_MAX",
            "rx_bounce_buf": "ESP_HOST_RX_AGGR_SIZE",
        }
        for member, size in expected.items():
            with self.subTest(buffer=member):
                self.assertRegex(
                    fn,
                    r"free_pages\(\(unsigned long\)context->"
                    + re.escape(member)
                    + r",\s*get_order\("
                    + re.escape(size)
                    + r"\)\)",
                )
                self.assertIn("context->" + member + " = NULL;", fn)


if __name__ == "__main__":
    unittest.main()
