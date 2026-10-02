#!/usr/bin/env bash
set -euo pipefail

IDF_TARGET="${1:?Usage: build-firmware.sh <idf-target> <sdio|spi|usb>}"
TRANSPORT="${2:?Usage: build-firmware.sh <idf-target> <sdio|spi|usb>}"

[[ "$TRANSPORT" == "sdio" || "$TRANSPORT" == "spi" || "$TRANSPORT" == "usb" ]] || {
    echo "ERROR: transport must be sdio, spi or usb" >&2
    exit 2
}

if [[ "$TRANSPORT" == "usb" && "$IDF_TARGET" != "esp32s31" ]]; then
    echo "ERROR: USB transport is supported only for esp32s31" >&2
    exit 2
fi

PROJECT_ROOT="${CI_PROJECT_DIR:-$(pwd)}"
DRIVER_ROOT="${PROJECT_ROOT}/esp/esp_driver"
APP_ROOT="${DRIVER_ROOT}/network_adapter"
ARTIFACT_DIR="${PROJECT_ROOT}/build-artifacts/firmware/${IDF_TARGET}/${TRANSPORT}"

: "${IDF_PATH:?IDF_PATH is not set by firmware CI image}"

# Read the IDF revision required by this repository.
# shellcheck disable=SC1091
source "${DRIVER_ROOT}/.env"

echo "=== Verifying CI image ==="

ACTUAL_IDF_COMMIT="$(git -C "$IDF_PATH" rev-parse HEAD)"

echo "Expected IDF commit: ${IDF_COMMIT}"
echo "Image IDF commit:    ${ACTUAL_IDF_COMMIT}"

if [[ "$ACTUAL_IDF_COMMIT" != "$IDF_COMMIT" ]]; then
    echo "ERROR: firmware CI image contains the wrong ESP-IDF revision"
    exit 1
fi

# setup.sh normally applies this patch. It must already exist in the CI image.
if ! git -C "$IDF_PATH" apply --reverse --check \
        "${DRIVER_ROOT}/lib/rom.patch"; then
    echo "ERROR: ESP-Hosted ROM patch is not applied in the firmware CI image"
    exit 1
fi

echo "ESP-IDF image validation passed"

#
# Only this part remains dynamic.
# Libraries must always come from the MR/branch being tested.
#
echo "=== Installing ESP-Hosted wireless libraries ==="

mkdir -p "$IDF_PATH/components/esp_wifi/lib"
rm -rf "$IDF_PATH/components/esp_wifi/lib/"*
cp -a "${DRIVER_ROOT}/lib/." "$IDF_PATH/components/esp_wifi/lib/"

#
# Use runner-provided persistent ccache.
#
export IDF_CCACHE_ENABLE=1
export CCACHE_NOHASHDIR=true

# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null

cd "$APP_ROOT"

rm -rf build sdkconfig sdkconfig.old

SDK_DEFAULTS="sdkconfig.defaults"
IDF_PREVIEW_ARGS=()

# ESP32-S31 is still a preview target in the pinned ESP-IDF v6.1 revision.
# Keep the flag scoped to S31 so stable targets retain their normal behavior.
if [[ "$IDF_TARGET" == "esp32s31" ]]; then
    IDF_PREVIEW_ARGS=(--preview)
fi

if [[ "$TRANSPORT" == "spi" ]]; then
    [[ -f sdkconfig.ci ]] || {
        echo "ERROR: sdkconfig.ci is required for SPI CI" >&2
        exit 1
    }

    if [[ "$IDF_TARGET" == "esp32s31" ]]; then
        [[ -f sdkconfig.defaults.esp32s31 ]] || {
            echo "ERROR: sdkconfig.defaults.esp32s31 is required for ESP32-S31 SPI" >&2
            exit 1
        }
        SDK_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s31;sdkconfig.ci"
    else
        SDK_DEFAULTS="sdkconfig.defaults;sdkconfig.ci"
    fi
elif [[ "$TRANSPORT" == "usb" ]]; then
    [[ -f sdkconfig.defaults.esp32s31_usb ]] || {
        echo "ERROR: sdkconfig.defaults.esp32s31_usb is required for USB" >&2
        exit 1
    }

    [[ -f sdkconfig.defaults.esp32s31 ]] || {
        echo "ERROR: sdkconfig.defaults.esp32s31 is required for USB" >&2
        exit 1
    }

    SDK_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s31;sdkconfig.defaults.esp32s31_usb"
fi

printf '%s\n' \
    "=== ESP firmware build ===" \
    "IDF          : $(idf.py --version)" \
    "IDF commit   : ${ACTUAL_IDF_COMMIT}" \
    "target       : ${IDF_TARGET}" \
    "transport    : ${TRANSPORT}" \
    "sdk defaults : ${SDK_DEFAULTS}"

idf.py "${IDF_PREVIEW_ARGS[@]}" \
    -D "SDKCONFIG_DEFAULTS=${SDK_DEFAULTS}" \
    set-target "$IDF_TARGET"

idf.py "${IDF_PREVIEW_ARGS[@]}" \
    -D "SDKCONFIG_DEFAULTS=${SDK_DEFAULTS}" \
    build

test -s build/network_adapter.bin

mkdir -p "$ARTIFACT_DIR"

cp build/network_adapter.bin "$ARTIFACT_DIR/"
[[ -f build/network_adapter.elf ]] && cp build/network_adapter.elf "$ARTIFACT_DIR/"
[[ -f build/network_adapter.map ]] && cp build/network_adapter.map "$ARTIFACT_DIR/"
cp sdkconfig "$ARTIFACT_DIR/"

BIN_SIZE="$(wc -c < build/network_adapter.bin | tr -d ' ')"
MAX_BIN_BYTES="$(python3 -c '
import sys
sys.path.insert(0, sys.argv[1] + "/components/partition_table")
from gen_esp32part import APP_TYPE, PartitionTable
table = PartitionTable.from_binary(open(sys.argv[2], "rb").read())
apps = [p.size for p in table if p.type == APP_TYPE]
if not apps:
    raise SystemExit("no app partition in table")
print(min(apps))
' "$IDF_PATH" build/partition_table/partition-table.bin)"

{
    echo "target=${IDF_TARGET}"
    echo "transport=${TRANSPORT}"
    echo "commit=${CI_COMMIT_SHA:-unknown}"
    echo "idf_commit=${ACTUAL_IDF_COMMIT}"
    echo "idf_version=$(idf.py --version)"
    echo "bin_size=${BIN_SIZE}"
    echo "app_partition_bytes=${MAX_BIN_BYTES}"
    echo "bin_sha256=$(sha256sum build/network_adapter.bin | awk '{print $1}')"
} > "$ARTIFACT_DIR/build-info.txt"

cat "$ARTIFACT_DIR/build-info.txt"

echo "network_adapter.bin: ${BIN_SIZE} bytes (app partition ${MAX_BIN_BYTES} bytes)"
if (( BIN_SIZE >= MAX_BIN_BYTES )); then
    echo "ERROR: ${IDF_TARGET}/${TRANSPORT} network_adapter.bin is ${BIN_SIZE} bytes; must be < ${MAX_BIN_BYTES} (smallest app partition)" >&2
    exit 1
fi

echo "=== PASS: ${IDF_TARGET}/${TRANSPORT} ==="
