#!/usr/bin/env python3
"""Compile actual production SPI no-I/O classification functions with C sanitizer."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
from run_usb_parser import extract_function, extract_typed_function

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "host/spi/esp_spi.c"
FW_SOURCE = ROOT / "esp/esp_driver/network_adapter/main/spi_slave_api.c"
HARNESS = ROOT / "tests/software/spi_parser_harness.c"
FUNCTIONS = ("esp_spi_bootstrap_frame_valid", "spi_get_cmd_info",
             "spi_is_stateful_packet", "spi_is_dummy_rx",
             "spi_is_unadvertised_idle_rx", "esp_spi_next_slot_ready")
HOST_INT_FUNCTIONS = ("process_rx_buf",)
FW_BOOL_FUNCTIONS = ("spi_tx_queues_pending", "spi_sync_dummy_slot_data_ready")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    src = SOURCE.read_text()
    fw_src = FW_SOURCE.read_text()
    harness = HARNESS.read_text()
    marker = "/* GENERATED_SPI_FUNCTIONS */"
    if harness.count(marker) != 1:
        raise ValueError("SPI harness marker changed")
    extracted = [extract_function(src, n) for n in FUNCTIONS]
    extracted.extend(extract_typed_function(src, n, "int")
                     for n in HOST_INT_FUNCTIONS)
    extracted.append(
        extract_typed_function(fw_src, "spi_validate_output_gpio", "esp_err_t")
    )
    extracted.extend(extract_function(fw_src, n) for n in FW_BOOL_FUNCTIONS)
    extracted.append(
        extract_typed_function(fw_src, "spi_post_setup_cb", "void IRAM_ATTR")
    )
    harness = harness.replace(marker, "\n\n".join(extracted))
    generated = out / "spi-production-parser.c"
    generated.write_text(harness)
    exe = out / "spi-production-parser"
    sanitizer = os.environ.get("SOFTWARE_SANITIZERS", "undefined")
    if sanitizer not in ("undefined", "address,undefined", "none"):
        raise ValueError("unsupported sanitizer")
    cmd = shlex.split(os.environ.get("CC", "gcc")) + [
        "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-fno-omit-frame-pointer", "-I", str(ROOT / "host/include"),
        str(generated), "-o", str(exe)]
    if sanitizer != "none":
        cmd += ["-fsanitize=" + sanitizer]
    for stage, argv in (("compile", cmd), ("execute", [str(exe)])):
        run = subprocess.run(argv, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=90, check=False)
        print(stage.upper() + ": " + " ".join(argv))
        print(run.stdout)
        if run.returncode:
            return run.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
