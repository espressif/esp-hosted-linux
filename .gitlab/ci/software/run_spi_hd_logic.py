#!/usr/bin/env python3
"""Extract production SPI-HD capability and ACK functions; compile and run C oracle."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

from run_usb_parser import extract_function, extract_typed_function

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "host/spi_hd/esp_spi_hd.c"
HARNESS = ROOT / "tests/software/spi_hd_batch_harness.c"
FUNCTIONS = (
    "spihd_batch_fits",
    "spihd_combined_wrdma",
    "spihd_rx_ack_required",
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    code = SOURCE.read_text()
    harness = HARNESS.read_text()
    marker = "/* GENERATED_SPIDH_FUNCTIONS */"
    if harness.count(marker) != 1:
        raise ValueError("SPI-HD harness marker changed")
    extracted = [extract_function(code, n) for n in FUNCTIONS]
    extracted.append(extract_typed_function(code, "spihd_effective_tx_segment", "u32"))
    extracted.append(extract_typed_function(code, "spihd_select_data_lines", "u8"))
    extracted.append(extract_typed_function(code, "spihd_phase_lane_supported", "bool"))
    extracted.append(extract_typed_function(code, "spihd_validate_rx_batch", "int"))
    extracted.append(extract_typed_function(code, "spihd_credit_delta", "u32"))
    extracted.append(extract_typed_function(code, "spihd_payload_lines", "u8"))
    harness = harness.replace(marker, "\n\n".join(extracted))
    source = out / "spi-hd-production-ack.c"
    source.write_text(harness)
    exe = out / "spi-hd-production-ack"
    sanitizer = os.environ.get("SOFTWARE_SANITIZERS", "undefined")
    if sanitizer not in ("none", "undefined", "address,undefined"):
        raise ValueError("unsupported sanitizer")
    cmd = shlex.split(os.environ.get("CC", "gcc")) + [
        "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-fno-omit-frame-pointer", "-I", str(ROOT / "host/include"),
        str(source), "-o", str(exe),
    ]
    if sanitizer != "none":
        cmd += ["-fsanitize=" + sanitizer]
    for stage, argv in (("compile", cmd), ("execute", [str(exe)])):
        result = subprocess.run(argv, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=90, check=False)
        print(stage.upper() + ": " + " ".join(argv))
        print(result.stdout)
        if result.returncode:
            return result.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
