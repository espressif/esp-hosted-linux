# SPDX-License-Identifier: Apache-2.0
"""Keep the peripheral transport independent of host-controller internals."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SpiHdControllerNeutralTests(unittest.TestCase):
    def test_peripheral_driver_has_no_controller_identity_workarounds(self):
        paths = (
            ROOT / "host/spi_hd/esp_spi_hd.c",
            ROOT / "host/spi_hd/esp_spi_hd.h",
            ROOT / "host/include/esp_spi_common.h",
        )
        for path in paths:
            text = path.read_text()
            # Check executable code and strings, not explanatory comments.
            code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
            with self.subTest(path=path):
                self.assertNotRegex(code, r"(?i)\b(?:rp1|rpi5|raspberrypi|bcm2712|designware|dw_spi\w*)\b")
                self.assertNotRegex(code, r"\b(?:spi_controller_get_devdata|spi_master_get_devdata)\s*\(")
                self.assertNotRegex(code, r"\b(?:readl|writel|ioremap|devm_ioremap)\s*\(")
        source = paths[0].read_text()
        self.assertIn("spi_sync_transfer(", source)
        self.assertIn("spi_max_transfer_size(", source)
        self.assertIn("spi_max_message_size(", source)


if __name__ == "__main__":
    unittest.main()
