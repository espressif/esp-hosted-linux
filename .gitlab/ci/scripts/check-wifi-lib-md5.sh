#!/usr/bin/env bash
# Same check IDF's test_md5.sh uses for esp_wifi_driver.h, and only that check.
#
# IDF:
#   check_md5 $IDF_PATH/components/wpa_supplicant/esp_supplicant/src/esp_wifi_driver.h \
#             g_wifi_supplicant_funcs_md5
# Hosted header:
#   esp/esp_driver/network_adapter/main/esp_wifi_driver.h
set -euo pipefail

if [ -z "${IDF_PATH:-}" ]; then
    echo "IDF_PATH must be set before running this script"
    exit 1
fi

REPO_ROOT="${CI_PROJECT_DIR:-$(git rev-parse --show-toplevel)}"
LIB_ROOT="${REPO_ROOT}/esp/esp_driver/lib"
HEADER="${REPO_ROOT}/esp/esp_driver/network_adapter/main/esp_wifi_driver.h"
ELF_FILE="test.elf"

if [ ! -f "${HEADER}" ]; then
    echo "ERROR: missing ${HEADER}" >&2
    exit 1
fi

# shellcheck disable=SC1091
source "${IDF_PATH}/export.sh" >/dev/null

toolchain_prefix() {
    case "$1" in
        esp32) echo xtensa-esp32-elf- ;;
        esp32s2) echo xtensa-esp32s2-elf- ;;
        esp32s3) echo xtensa-esp32s3-elf- ;;
        esp32c2|esp32c3|esp32c6|esp32c5|esp32_host|esp32c61|esp32s31)
            echo riscv32-esp-elf-
            ;;
        *)
            echo "Invalid IDF_TARGET value: \"$1\"" >&2
            return 1
            ;;
    esac
}

# Copied from esp-idf components/esp_wifi/test_md5/test_md5.sh
check_md5()
{
    FILENAME=$1
    SYMBOL=$2

    GDB_COMMAND="printf \"%s\\n\", (const char*) ${SYMBOL}"
    MD5_FROM_LIB=$(${PREFIX}gdb -n -batch ${ELF_FILE} -ex "${GDB_COMMAND}")
    MD5_FROM_HEADER=$(md5sum ${FILENAME} | cut -c 1-7)

    echo "Checking ${FILENAME}:"
    echo "  ${MD5_FROM_HEADER} - from header file"
    echo "  ${MD5_FROM_LIB} - from library"
    if [ "${MD5_FROM_LIB}" != "${MD5_FROM_HEADER}" ]; then
        echo "  error: MD5 mismatch!"
        FAILURES=$(($FAILURES+1))
    fi
}

FAILURES=0
found_target=0
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

while IFS= read -r dir; do
    shopt -s nullglob
    archives=("${dir}"/*.a)
    shopt -u nullglob
    (( ${#archives[@]} > 0 )) || continue

    IDF_TARGET="$(basename "$dir")"
    PREFIX="$(toolchain_prefix "$IDF_TARGET")" || {
        FAILURES=$((FAILURES+1))
        continue
    }

    found_target=1
    ELF_FILE="${workdir}/${IDF_TARGET}-test.elf"

    echo "Checking libraries for target ${IDF_TARGET}..."
    ${PREFIX}ld --unresolved-symbols=ignore-all --entry 0 -o ${ELF_FILE} \
        -u g_wifi_supplicant_funcs_md5 \
        "${archives[@]}"

    check_md5 "${HEADER}" g_wifi_supplicant_funcs_md5
done < <(find "${LIB_ROOT}" -mindepth 1 -maxdepth 1 -type d -name 'esp32*' | sort)

if [ "$found_target" -eq 0 ]; then
    echo "ERROR: no chip library archives found under ${LIB_ROOT}" >&2
    exit 1
fi

if [ $FAILURES -gt 0 ]; then
    exit 1
fi
