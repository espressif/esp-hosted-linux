"""ARMv7 SPI-HD must not emit a compiler runtime 64-bit division helper.

Static source regression; real ARMv7 kernel module/link qualification happens
in the user-run Linux 6.1/4.9 SDK build and is NOT implied by this test.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPI_HD = ROOT / "host/spi_hd/esp_spi_hd.c"


class SpiHdArmv7MathTests(unittest.TestCase):
    def test_timing_conversions_use_kernel_math64(self):
        code = SPI_HD.read_text()
        self.assertIn("#include <linux/math64.h>", code)
        # Timing code must use kernel-safe arithmetic, independent of count.
        self.assertNotRegex(
            code,
            r"\(ktime_get_ns\(\)\s*-\s*started\)\s*/\s*1000\b",
        )

    def test_no_new_unchecked_64bit_timing_division(self):
        code = SPI_HD.read_text()
        # Maintain compiler-runtime independence on 32-bit kernels.
        for line in code.splitlines():
            if "ktime_get_ns()" in line:
                self.assertNotRegex(line, r"\)\s*/\s*[0-9]+\b")


if __name__ == "__main__":
    unittest.main()
