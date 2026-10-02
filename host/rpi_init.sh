#!/usr/bin/env bash

# SPDX-License-Identifier: Apache-2.0
# Copyright 2015-2026 Espressif Systems (Shanghai) CO LTD

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

IF_TYPE="sdio"
BT_INIT_SET="0"
RAW_TP_MODE="0"
AP_SUPPORT="0"
OTA_FILE=""

# Historical Raspberry Pi wiring defaults. Override these for another board.
SPI_RESETGPIO="6"
SPI_HANDSHAKEGPIO="22"
SPI_DATAREADYGPIO="27"
SPI_MAX_FREQUENCY="30000000"
SPI_CS_CHANGE="0"

if [ "$(id -u)" -eq 0 ]; then
    SUDO=()
else
    SUDO=(sudo)
fi

run_root()
{
    "${SUDO[@]}" "$@"
}

die()
{
    echo "ERROR: $*" >&2
    exit 1
}

module_loaded()
{
    grep -q "^$1 " /proc/modules 2>/dev/null
}

overlay_id()
{
    local name="$1"

    command -v dtoverlay >/dev/null 2>&1 || return 1
    dtoverlay -l 2>/dev/null |
        awk -F: -v name="$name" '
            {
                id=$1
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", id)
                rest=$2
                sub(/^[[:space:]]+/, "", rest)
                split(rest, fields, /[[:space:]]+/)
                if (fields[1] == name) {
                    print id
                    exit
                }
            }'
}

remove_runtime_overlay()
{
    local name="$1"
    local id=""

    id="$(overlay_id "$name" || true)"
    if [ -n "$id" ]; then
        echo "Removing runtime overlay $name (id $id)"
        run_root dtoverlay -r "$id"
    fi
}

require_gpio()
{
    local name="$1"
    local value="$2"

    case "$value" in
        ''|*[!0-9]*)
            die "$name must be a BCM GPIO number"
            ;;
    esac

    [ "$value" -le 53 ] || die "$name GPIO$value is outside BCM GPIO0..53"

    # SPI0 itself owns GPIO7..11 on the classic Raspberry Pi header.
    if [ "$value" -ge 7 ] && [ "$value" -le 11 ]; then
        die "$name GPIO$value conflicts with SPI0"
    fi
}

wait_for_spi_bind()
{
    local retries=20
    local driver=""

    while [ "$retries" -gt 0 ]; do
        if [ -L /sys/bus/spi/devices/spi0.0/driver ]; then
            driver="$(basename "$(readlink -f /sys/bus/spi/devices/spi0.0/driver)")"
            [ "$driver" = "esp_spi" ] && return 0
        fi
        sleep 0.25
        retries=$((retries - 1))
    done
    return 1
}

bringup_network_interface()
{
    local retries=20

    while [ "$retries" -gt 0 ]; do
        if [ -e /sys/class/net/wlan0 ]; then
            if command -v ip >/dev/null 2>&1; then
                run_root ip link set wlan0 up
            elif command -v ifconfig >/dev/null 2>&1; then
                run_root ifconfig wlan0 up
            fi
            return 0
        fi
        sleep 0.25
        retries=$((retries - 1))
    done

    return 1
}

build_driver()
{
    local arch_found
    local -a make_args

    if [ "$(getconf LONG_BIT)" = "32" ]; then
        arch_found="arm"
    else
        arch_found="arm64"
    fi

    make_args=(
        "target=$IF_TYPE"
        "KERNEL=/lib/modules/$(uname -r)/build"
        "ARCH=$arch_found"
    )

    if [ "$AP_SUPPORT" = "1" ]; then
        make_args+=("CONFIG_AP_SUPPORT=y")
    fi

    echo "Building for $IF_TYPE protocol"
    make -j8 "${make_args[@]}"

    if [ "$IF_TYPE" = "spi" ]; then
        command -v dtc >/dev/null 2>&1 || die "dtc is required for SPI Device Tree setup"
        command -v dtoverlay >/dev/null 2>&1 || die "dtoverlay is required for SPI Device Tree setup"
        make spi-dtbo
    fi
}

unload_esp_modules()
{
    # rpi_init.sh is a bring-up helper: replace whichever ESP transport is
    # currently loaded instead of maintaining rollback/provenance state.
    if module_loaded esp32_sdio; then
        echo "Unloading esp32_sdio"
        run_root rmmod esp32_sdio
    fi
    if module_loaded esp32_spi; then
        echo "Unloading esp32_spi"
        run_root rmmod esp32_spi
    fi
    if module_loaded esp32_usb; then
        echo "Unloading esp32_usb"
        run_root rmmod esp32_usb
    fi
}

setup_spi()
{
    local -a module_args

    require_gpio resetgpio "$SPI_RESETGPIO"
    require_gpio handshakegpio "$SPI_HANDSHAKEGPIO"
    require_gpio datareadygpio "$SPI_DATAREADYGPIO"

    if [ "$SPI_RESETGPIO" = "$SPI_HANDSHAKEGPIO" ] ||
       [ "$SPI_RESETGPIO" = "$SPI_DATAREADYGPIO" ] ||
       [ "$SPI_HANDSHAKEGPIO" = "$SPI_DATAREADYGPIO" ]; then
        die "resetgpio, handshakegpio and datareadygpio must be distinct"
    fi

    case "$SPI_MAX_FREQUENCY" in
        ''|*[!0-9]*) die "max_frequency must be an integer in Hz" ;;
    esac
    [ "$SPI_MAX_FREQUENCY" -gt 0 ] || die "max_frequency must be greater than zero"
    [ "$SPI_MAX_FREQUENCY" -le 40000000 ] ||
        die "max_frequency exceeds the ESP-Hosted SPI 40 MHz limit"

    case "$SPI_CS_CHANGE" in
        0|1) ;;
        *) die "spi_cs_change must be 0 or 1" ;;
    esac

    # Remove only overlays owned by this helper. Do not touch the Raspberry Pi
    # SDIO boot overlay: custom SPI control GPIOs can coexist with it.
    remove_runtime_overlay esp32-spi
    remove_runtime_overlay spidev_disabler

    module_args=("raw_tp_mode=$RAW_TP_MODE" "spi_cs_change=$SPI_CS_CHANGE")
    if [ -n "$OTA_FILE" ]; then
        module_args+=("ota_file=$OTA_FILE")
    fi

    # Register our freshly built driver before creating the DT child so udev
    # cannot win a modalias race with an older installed esp32_spi.ko.
    run_root insmod ./esp32_spi.ko "${module_args[@]}"

    echo "Disabling stock spidev0 Device Tree node"
    run_root dtoverlay -d "$SCRIPT_DIR" spidev_disabler

    echo "Applying ESP SPI Device Tree overlay:"
    echo "  reset=$SPI_RESETGPIO handshake=$SPI_HANDSHAKEGPIO data-ready=$SPI_DATAREADYGPIO max-frequency=$SPI_MAX_FREQUENCY"
    run_root dtoverlay -d "$SCRIPT_DIR/overlays" esp32-spi         "resetgpio=$SPI_RESETGPIO"         "handshakegpio=$SPI_HANDSHAKEGPIO"         "datareadygpio=$SPI_DATAREADYGPIO"         "max_frequency=$SPI_MAX_FREQUENCY"

    # Normally the driver core binds immediately when the DT child appears.
    # Give it a short chance, then issue one explicit probe for bring-up kernels
    # where auto-probe was disabled externally.
    if ! wait_for_spi_bind; then
        if [ -e /sys/bus/spi/devices/spi0.0 ] &&
           [ -e /sys/bus/spi/drivers_probe ]; then
            printf 'spi0.0\n' | run_root tee /sys/bus/spi/drivers_probe >/dev/null || true
        fi
    fi

    wait_for_spi_bind ||
        die "esp32_spi loaded but spi0.0 did not bind; check dmesg for the probe error"
}

setup_sdio()
{
    local -a module_args

    module_args=("raw_tp_mode=$RAW_TP_MODE")
    if [ -n "$OTA_FILE" ]; then
        module_args+=("ota_file=$OTA_FILE")
    fi

    run_root insmod ./esp32_sdio.ko "${module_args[@]}"
}

setup_usb()
{
    local -a module_args

    module_args=("raw_tp_mode=$RAW_TP_MODE")
    if [ -n "$OTA_FILE" ]; then
        module_args+=("ota_file=$OTA_FILE")
    fi

    # USB needs no Raspberry Pi GPIO or Device Tree setup. Loading the module
    # is sufficient; the USB core binds it when the ESP32-S31 enumerates as
    # 303a:4002.
    run_root insmod ./esp32_usb.ko "${module_args[@]}"
}

wlan_init()
{
    build_driver
    unload_esp_modules

    case "$IF_TYPE" in
        spi)
            setup_spi
            ;;
        sdio)
            setup_sdio
            ;;
        usb)
            setup_usb
            ;;
        *)
            die "unsupported transport: $IF_TYPE"
            ;;
    esac

    if ! bringup_network_interface; then
        echo "WARNING: module loaded but wlan0 did not appear yet" >&2
        echo "Check dmesg for firmware/transport bring-up logs." >&2
    fi

    echo "ESP32 host init completed"
}

bt_init()
{
    run_root pinctrl set 15 a0 pu
    run_root pinctrl set 14 a0 pu
    if [ "$BT_INIT_SET" = "4" ]; then
        run_root pinctrl set 16 a3 pu
        run_root pinctrl set 17 a3 pu
    fi
}

usage()
{
    cat <<'EOF'
This script prepares a Raspberry Pi for ESP-Hosted bring-up.

Usage:
  ./rpi_init.sh [spi|sdio|usb] [options]

Common options:
  btuart | btuart_4pins
  btuart_2pins
  rawtp_host_to_esp
  rawtp_esp_to_host
  ap_support
  ota_file=/path/to/file

SPI Device Tree options:
  resetgpio=N           ESP EN/reset BCM GPIO (default: 6)
  handshakegpio=N       ESP handshake BCM GPIO (default: 22)
  datareadygpio=N       ESP data-ready BCM GPIO (default: 27)
  max_frequency=N       spi-max-frequency in Hz (default: 30000000)
  spi_cs_change=0|1     Optional controller workaround (default: 0)

Legacy:
  resetpin=N            Alias for resetgpio=N when using SPI

Examples:
  ./rpi_init.sh sdio
  ./rpi_init.sh spi
  ./rpi_init.sh usb
  ./rpi_init.sh spi handshakegpio=5 datareadygpio=12
  ./rpi_init.sh spi resetgpio=6 handshakegpio=5 datareadygpio=12
EOF
}

parse_arguments()
{
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --help|-h)
                usage
                exit 0
                ;;
            spi|sdio|usb)
                IF_TYPE="$1"
                ;;
            resetgpio=*)
                SPI_RESETGPIO="${1#*=}"
                ;;
            resetpin=*)
                SPI_RESETGPIO="${1#*=}"
                ;;
            handshakegpio=*)
                SPI_HANDSHAKEGPIO="${1#*=}"
                ;;
            datareadygpio=*)
                SPI_DATAREADYGPIO="${1#*=}"
                ;;
            max_frequency=*)
                SPI_MAX_FREQUENCY="${1#*=}"
                ;;
            spi_cs_change=*)
                SPI_CS_CHANGE="${1#*=}"
                ;;
            btuart|btuart_4pins|btuart_4pin)
                BT_INIT_SET="4"
                ;;
            btuart_2pins|btuart_2pin)
                BT_INIT_SET="2"
                ;;
            rawtp_host_to_esp)
                RAW_TP_MODE="1"
                ;;
            rawtp_esp_to_host)
                RAW_TP_MODE="2"
                ;;
            ap_support)
                AP_SUPPORT="1"
                ;;
            ota_file=*)
                OTA_FILE="${1#*=}"
                ;;
            *)
                echo "$1: unknown option" >&2
                usage
                exit 1
                ;;
        esac
        shift
    done
}

parse_arguments "$@"

run_root modprobe bluetooth
run_root modprobe cfg80211

wlan_init

if [ "$BT_INIT_SET" != "0" ]; then
    bt_init
fi
