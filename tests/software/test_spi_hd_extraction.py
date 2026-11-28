"""Ensure the SPI-HD runtime C checks actually execute extracted driver code."""
from pathlib import Path
import importlib.util
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / ".gitlab/ci/software"
sys.path.insert(0, str(SCRIPTS))
from run_usb_parser import extract_function

class SpiHdExtractionTests(unittest.TestCase):
    def test_batch_and_ack_paths_are_real_functions(self):
        source = (ROOT / "host/spi_hd/esp_spi_hd.c").read_text()
        for name in ("spihd_batch_fits", "spihd_combined_wrdma", "spihd_rx_ack_required"):
            with self.subTest(name=name):
                body = extract_function(source, name)
                self.assertIn("return ", body)
        self.assertIn("spihd_combined_wrdma(len, wr_lines)", source)
        self.assertIn("spihd_rx_ack_required(status, pending)", source)

    def test_missing_ack_fails_extractor(self):
        with self.assertRaises(ValueError):
            extract_function("static bool spihd_other(void) { return true; }",
                             "spihd_rx_ack_required")


if __name__ == "__main__":
    unittest.main()
