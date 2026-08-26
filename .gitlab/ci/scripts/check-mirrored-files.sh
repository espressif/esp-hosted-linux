#!/usr/bin/env bash
set -euo pipefail

check_same() {
    local host_file="$1"
    local fw_file="$2"

    [[ -f "$host_file" ]] || { echo "ERROR: missing $host_file" >&2; exit 1; }
    [[ -f "$fw_file" ]] || { echo "ERROR: missing $fw_file" >&2; exit 1; }

    echo "Checking mirrored files:"
    echo "  host     : $host_file"
    echo "  firmware : $fw_file"

    if ! cmp -s "$host_file" "$fw_file"; then
        echo "ERROR: mirrored host/firmware files differ" >&2
        diff -u "$host_file" "$fw_file" || true
        exit 1
    fi
}

check_same \
    host/include/esp_fw_version.h \
    esp/esp_driver/network_adapter/main/include/esp_fw_version.h

check_same \
    host/include/adapter.h \
    esp/esp_driver/network_adapter/main/include/adapter.h

echo "Mirrored host/firmware files are synchronized."
