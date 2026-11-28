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
SPI_DATAREADYGPIO_SET="0"
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
    # dtoverlay -l can crash without root permissions for root-owned DTBOs.
    # Never mistake a failed enumeration for an empty overlay registry.
    local registry=""
    registry="$(run_root dtoverlay -l)" || return 1
    printf '%s\n' "$registry" |
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

    id="$(overlay_id "$name")" ||
        die "Cannot enumerate privileged overlay registry for $name"
    if [ -n "$id" ]; then
        echo "Removing runtime overlay $name (id $id)"
        run_root dtoverlay -r "$id"
        id="$(overlay_id "$name")" ||
            die "Cannot verify runtime overlay cleanup for $name"
        [ -z "$id" ] ||
            die "Runtime overlay $name remains registered after removal"
    fi
}

rpi_compatible_has()
{
    local compat="$1"

    [ -r /proc/device-tree/compatible ] || return 1
    tr '\0' '\n' </proc/device-tree/compatible | grep -qx "$compat"
}

rpi_detected()
{
    [ -r /proc/device-tree/compatible ] || return 1
    tr '\0' '\n' </proc/device-tree/compatible | grep -q '^raspberrypi,'
}

rpi3b_plus_detected()
{
    rpi_compatible_has "raspberrypi,3-model-b-plus" &&
        rpi_compatible_has "brcm,bcm2837"
}

require_rpi_overlay_host()
{
    rpi_detected ||
        die "Raspberry Pi Device Tree overlays are only supported on Raspberry Pi; describe the ESP SPI node in the target board DT instead"
}

spi_overlay_name()
{
    if rpi3b_plus_detected; then
        printf '%s\n' esp32-spi-rpi3b-plus
    else
        printf '%s\n' esp32-spi
    fi
}

esp_sdio_device_present()
{
    local dev=""
    local vendor=""
    local device=""

    for dev in /sys/bus/sdio/devices/*:*:*; do
        [ -e "$dev" ] || continue
        vendor="$(cat "$dev/vendor" 2>/dev/null || true)"
        device="$(cat "$dev/device" 2>/dev/null || true)"
        case "$vendor:$device" in
            0x6666:0x2222|0x0092:0x6666)
                return 0
                ;;
        esac
    done
    return 1
}

sdio_boot_full_chip_reset_declared()
{
    local tree=""

    # /proc/device-tree is commonly a symlink. Resolve it so find actually
    # descends the live tree instead of testing only the symlink itself.
    tree="$(readlink -f /proc/device-tree 2>/dev/null || true)"
    if [ -z "$tree" ] || [ ! -d "$tree" ]; then
        tree="/sys/firmware/devicetree/base"
    fi
    [ -d "$tree" ] || return 1
    find "$tree" -type f -name 'espressif,full-chip-reset' -print -quit 2>/dev/null |
        grep -q .
}

dt_gpio_cell_lows()
{
    local prop="$1"

    [ -r "$prop" ] || return 1
    # Raspberry Pi GPIO numbers fit in the low byte of each big-endian DT
    # cell. Emit that byte for every cell without requiring fdtget/hexdump.
    od -An -v -tu1 "$prop" 2>/dev/null |
        awk '{ for (i = 1; i <= NF; i++) { n++; if (n % 4 == 0) printf "%s%s", sep, $i; sep = " " } }
             END { if (n) print "" }'
}

rpi_sdio_boot_overlay_active()
{
    local tree=""

    tree="$(readlink -f /proc/device-tree 2>/dev/null || true)"
    if [ -z "$tree" ] || [ ! -d "$tree" ]; then
        tree="/sys/firmware/devicetree/base"
    fi
    [ -d "$tree" ] || return 1
    find "$tree" -type d -name 'sdio_ovl_pins' -print -quit 2>/dev/null |
        grep -q .
}

rpi_sdio_active_pins()
{
    local tree=""
    local node=""
    local pins=""
    local nodes=""

    tree="$(readlink -f /proc/device-tree 2>/dev/null || true)"
    if [ -z "$tree" ] || [ ! -d "$tree" ]; then
        tree="/sys/firmware/devicetree/base"
    fi
    [ -d "$tree" ] || return 1

    nodes="$(find "$tree" -type d -name 'sdio_ovl_pins' -print 2>/dev/null || true)"
    [ -n "$nodes" ] || return 1
    for node in $nodes; do
        local node_pins=""

        [ -r "$node/brcm,pins" ] || continue
        node_pins="$(dt_gpio_cell_lows "$node/brcm,pins" || true)"
        [ -n "$node_pins" ] || continue
        pins="${pins:+$pins }$node_pins"
    done
    [ -n "$pins" ] || return 1
    echo "$pins"
}

rpi_sdio_pwrseq_reset_pins()
{
    local tree=""
    local node=""
    local cells=""
    local nodes=""

    tree="$(readlink -f /proc/device-tree 2>/dev/null || true)"
    if [ -z "$tree" ] || [ ! -d "$tree" ]; then
        tree="/sys/firmware/devicetree/base"
    fi
    [ -d "$tree" ] || return 1

    nodes="$(find "$tree" -type d -name 'esp-sdio-pwrseq' -print 2>/dev/null || true)"
    [ -n "$nodes" ] || return 1
    for node in $nodes; do
        [ -r "$node/reset-gpios" ] || continue
        cells="$(dt_gpio_cell_lows "$node/reset-gpios" || true)"
        set -- $cells
        [ "$#" -ge 2 ] && echo "$2"
    done
}

rpi_sdio_conflicts_with_spi()
{
    local sdio_pins=""
    local reset_pins=""
    local pin=""
    local spi_pin=""

    sdio_pins="$(rpi_sdio_active_pins || true)"
    reset_pins="$(rpi_sdio_pwrseq_reset_pins || true)"
    [ -n "$sdio_pins$reset_pins" ] || return 1

    # SPI0 owns GPIO7..11. The ESP overlay additionally owns the selected
    # reset/handshake/data-ready pins. Refuse only on a real overlap; alternate
    # Raspberry Pi SDIO groups such as GPIO34..39 can coexist with SPI when
    # their pwrseq reset GPIO and the selected ESP control GPIOs are distinct.
    for pin in $sdio_pins $reset_pins; do
        for spi_pin in 7 8 9 10 11 \
            "$SPI_RESETGPIO" "$SPI_HANDSHAKEGPIO" "$SPI_DATAREADYGPIO"; do
            [ "$pin" = "$spi_pin" ] && return 0
        done
    done
    return 1
}

sdio_esp_bound()
{
    local dev=""

    for dev in /sys/bus/sdio/drivers/esp32_sdio/*:*:*; do
        [ -e "$dev" ] && return 0
    done
    return 1
}

wait_for_sdio_bind()
{
    local retries=20

    while [ "$retries" -gt 0 ]; do
        sdio_esp_bound && return 0
        sleep 0.25
        retries=$((retries - 1))
    done
    return 1
}

sdio_full_chip_reset_declared()
{
    local dev=""
    local path=""

    # Verify the marker only on the ancestry of an SDIO function actually bound
    # to esp32_sdio. A global /proc/device-tree search can report a different
    # MMC controller and falsely claim that ESP reset ownership is active.
    for dev in /sys/bus/sdio/drivers/esp32_sdio/*:*:*; do
        [ -e "$dev" ] || continue
        path="$(readlink -f "$dev" 2>/dev/null || true)"
        while [ -n "$path" ] && [ "$path" != "/" ]; do
            if [ -e "$path/of_node/espressif,full-chip-reset" ]; then
                return 0
            fi
            path="${path%/*}"
            [ -n "$path" ] || path="/"
        done
    done
    return 1
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
    local expected_driver="$1"
    local retries=20
    local driver=""

    while [ "$retries" -gt 0 ]; do
        if [ -L /sys/bus/spi/devices/spi0.0/driver ]; then
            driver="$(basename "$(readlink -f /sys/bus/spi/devices/spi0.0/driver)")"
            [ "$driver" = "$expected_driver" ] && return 0
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
    if [ "${ESP_HOSTED_INCREMENTAL:-0}" = "1" ]; then
        echo "Reusing existing Kbuild objects (ESP_HOSTED_INCREMENTAL=1)"
        make -j8 "${make_args[@]}" incremental
    else
        make -j8 "${make_args[@]}"
    fi

    if [ "$IF_TYPE" = "spi" ] || [ "$IF_TYPE" = "spi_hd" ]; then
        require_rpi_overlay_host
        command -v dtc >/dev/null 2>&1 || die "dtc is required for SPI Device Tree setup"
        command -v dtoverlay >/dev/null 2>&1 || die "dtoverlay is required for SPI Device Tree setup"
        make spi-dtbo
    elif [ "$IF_TYPE" = "sdio" ]; then
        # SDIO remains fully usable with its acknowledged in-band reboot path.
        # Building the optional boot-time EN-reset overlay must not turn dtc
        # into a new prerequisite for ordinary SDIO bring-up.
        if command -v dtc >/dev/null 2>&1; then
            make sdio-dtbo
        else
            echo "NOTE: dtc not found; skipping optional esp32-sdio-pwrseq.dtbo build" >&2
        fi
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
    if module_loaded esp32_spi_hd; then
        echo "Unloading esp32_spi_hd"
        run_root rmmod esp32_spi_hd
    fi
    if module_loaded esp32_usb; then
        echo "Unloading esp32_usb"
        run_root rmmod esp32_usb
    fi
}

setup_spi()
{
    local -a module_args
    local overlay_name

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

    # Only runtime SPI overlays are safe to replace here. A boot-time SDIO
    # configuration owns the MMC pinmux (and, with our pwrseq overlay, ESP EN)
    # before this script runs; it cannot be safely repurposed into SPI at runtime.
    overlay_name="$(spi_overlay_name)"
    remove_runtime_overlay esp32-spi
    remove_runtime_overlay esp32-spi-rpi3b-plus
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
    echo "  overlay=$overlay_name"
    echo "  reset=$SPI_RESETGPIO handshake=$SPI_HANDSHAKEGPIO data-ready=$SPI_DATAREADYGPIO max-frequency=$SPI_MAX_FREQUENCY"
    run_root dtoverlay -d "$SCRIPT_DIR/overlays" "$overlay_name"         "resetgpio=$SPI_RESETGPIO"         "handshakegpio=$SPI_HANDSHAKEGPIO"         "datareadygpio=$SPI_DATAREADYGPIO"         "max_frequency=$SPI_MAX_FREQUENCY"

    # Normally the driver core binds immediately when the DT child appears.
    # Give it a short chance, then issue one explicit probe for bring-up kernels
    # where auto-probe was disabled externally.
    if ! wait_for_spi_bind esp_spi; then
        if [ -e /sys/bus/spi/devices/spi0.0 ] &&
           [ -e /sys/bus/spi/drivers_probe ]; then
            printf 'spi0.0\n' | run_root tee /sys/bus/spi/drivers_probe >/dev/null || true
        fi
    fi

    wait_for_spi_bind esp_spi ||
        die "esp32_spi loaded but spi0.0 did not bind; check dmesg for the probe error"
}

setup_spi_hd()
{
    local -a module_args

    if [ ! -r /proc/device-tree/compatible ] ||
       ! tr '\0' '\n' </proc/device-tree/compatible | grep -qx 'brcm,bcm2712'; then
        die "SPI-HD/QIO requires an RP1/BCM2712 Raspberry Pi (Pi 5-class). Pi 4 and earlier onboard SPI controllers do not support quad SPI."
    fi

    require_gpio resetgpio "$SPI_RESETGPIO"
    require_gpio datareadygpio "$SPI_DATAREADYGPIO"

    [ "$SPI_RESETGPIO" != "$SPI_DATAREADYGPIO" ] ||
        die "resetgpio and datareadygpio must be distinct"

    # RP1 SPI0 QIO consumes GPIO0/1 and GPIO8..11.
    case "$SPI_RESETGPIO:$SPI_DATAREADYGPIO" in
        0:*|1:*|8:*|9:*|10:*|11:*|*:0|*:1|*:8|*:9|*:10|*:11)
            die "SPI-HD control GPIOs must not overlap RP1 SPI0 QIO GPIO0/1/8/9/10/11"
            ;;
    esac

    case "$SPI_MAX_FREQUENCY" in
        ''|*[!0-9]*) die "max_frequency must be an integer in Hz" ;;
    esac
    [ "$SPI_MAX_FREQUENCY" -gt 0 ] || die "max_frequency must be greater than zero"
    [ "$SPI_MAX_FREQUENCY" -le 40000000 ] ||
        die "max_frequency exceeds the initial SPI-HD 40 MHz qualification limit"


    remove_runtime_overlay esp32-spi-hd-rpi5
    remove_runtime_overlay esp32-spi
    remove_runtime_overlay spidev_disabler

    module_args=()
    [ "$RAW_TP_MODE" = "0" ] || module_args+=("raw_tp_mode=$RAW_TP_MODE")
    if [ -n "$OTA_FILE" ]; then
        module_args+=("ota_file=$OTA_FILE")
    fi

    if ! grep -qw spi_dw /proc/modules || ! grep -qw spi_dw_mmio /proc/modules; then
        echo "WARNING: DesignWare SPI modules are not loaded." >&2
    fi
    echo "NOTE: Raspberry Pi kernel support equivalent to linux#7587 is required."
    echo "      Use: bash rpi5_spi_hd/build-rp1-quad-modules.sh --load"

    run_root insmod ./esp32_spi_hd.ko "${module_args[@]}"

    echo "Disabling stock spidev0 Device Tree node"
    run_root dtoverlay -d "$SCRIPT_DIR" spidev_disabler

    # The runtime overlay extends the existing RP1 SPI0 pin group with
    # GPIO0/GPIO1 (SIO3/SIO2).  On an already-bound SPI0 controller, changing
    # the pin group's 'pins' property does not necessarily cause pinctrl to
    # re-select the state.  Force the two additional QIO lanes to SPI0 before
    # the ESP child is created.  GPIO0=a0 is SPI0_SIO3 and GPIO1=a0 is
    # SPI0_SIO2 on RP1.
    command -v pinctrl >/dev/null 2>&1 ||
        die "pinctrl is required to select RP1 SPI0 SIO2/SIO3 for runtime QIO bring-up"
    echo "Selecting RP1 SPI0 QIO extra lanes: GPIO0=SIO3 GPIO1=SIO2"
    run_root pinctrl set 0 a0 pn
    run_root pinctrl set 1 a0 pn
    run_root pinctrl get 0
    run_root pinctrl get 1

    echo "Applying Raspberry Pi 5 ESP SPI-HD/QIO overlay:"
    echo "  reset=$SPI_RESETGPIO data-ready=$SPI_DATAREADYGPIO max-frequency=$SPI_MAX_FREQUENCY"
    run_root dtoverlay -d "$SCRIPT_DIR/overlays" esp32-spi-hd-rpi5 \
        "resetgpio=$SPI_RESETGPIO" \
        "datareadygpio=$SPI_DATAREADYGPIO" \
        "max_frequency=$SPI_MAX_FREQUENCY"

    if ! wait_for_spi_bind esp_spi_hd; then
        if [ -e /sys/bus/spi/devices/spi0.0 ] &&
           [ -e /sys/bus/spi/drivers_probe ]; then
            printf 'spi0.0\n' | run_root tee /sys/bus/spi/drivers_probe >/dev/null || true
        fi
    fi

    wait_for_spi_bind esp_spi_hd ||
        die "esp32_spi_hd loaded but spi0.0 did not bind; check dmesg and verify the #7587-compatible RP1 modules are loaded"
}

setup_sdio()
{
    local -a module_args

    module_args=("raw_tp_mode=$RAW_TP_MODE")
    if [ -n "$OTA_FILE" ]; then
        module_args+=("ota_file=$OTA_FILE")
    fi

    run_root insmod ./esp32_sdio.ko "${module_args[@]}"

    if ! wait_for_sdio_bind; then
        echo "NOTE: ESP SDIO function did not bind within 5 seconds; reset DT contract cannot be verified" >&2
    elif sdio_full_chip_reset_declared; then
        echo "SDIO full-chip reset marker is active on the bound ESP MMC host"
    else
        echo "WARNING: bound ESP SDIO host has no active full-chip-reset DT contract." >&2
        echo "         Recovery will use the generation-ACKed software reboot fallback." >&2
        echo "         For DT EN reset, install overlays/esp32-sdio-pwrseq.dtbo in the" >&2
        echo "         Raspberry Pi boot overlays directory, add these before reboot:" >&2
        echo "           dtoverlay=esp32-sdio-pwrseq" >&2
        echo "           dtoverlay=sdio,poll_once=off" >&2
        echo "         Then reboot before running this helper again." >&2
    fi
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
    if [ "$IF_TYPE" = "spi" ] || [ "$IF_TYPE" = "spi_hd" ]; then
        require_rpi_overlay_host
    fi

    if [ "$IF_TYPE" = "spi" ]; then
        # Refuse before unloading anything when the live SDIO boot pinmux or
        # pwrseq reset GPIO actually overlaps SPI0/control GPIOs. Alternate
        # Raspberry Pi SDIO pin groups may coexist when they are disjoint.
        if rpi_sdio_conflicts_with_spi; then
            die "Raspberry Pi SDIO boot GPIOs overlap the selected SPI configuration; change one side or disable the SDIO boot overlays and reboot"
        fi
        # Presence without a parseable GPIO map is not evidence of safety.
        if rpi_sdio_boot_overlay_active &&
           ! rpi_sdio_active_pins >/dev/null; then
            die "Raspberry Pi SDIO pinmux is active but its GPIO map cannot be verified; disable the SDIO boot configuration and reboot before selecting SPI"
        fi
        if sdio_boot_full_chip_reset_declared &&
           ! rpi_sdio_pwrseq_reset_pins >/dev/null; then
            die "ESP SDIO full-chip reset is active but its reset GPIO cannot be verified; disable the SDIO boot configuration and reboot before selecting SPI"
        fi
        # If an ESP SDIO session/contract is active but the helper cannot see
        # the stock Pi pin node, its data-pin ownership is unknown: fail closed.
        if { esp_sdio_device_present || sdio_boot_full_chip_reset_declared; } &&
           ! rpi_sdio_boot_overlay_active; then
            die "ESP SDIO is active but its boot pinmux cannot be verified; disable the SDIO boot configuration and reboot before selecting SPI"
        fi
    fi

    build_driver
    unload_esp_modules

    case "$IF_TYPE" in
        spi)
            setup_spi
            ;;
        spi_hd)
            setup_spi_hd
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
  ./rpi_init.sh [spi|spi_hd|sdio|usb] [options]

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
  datareadygpio=N       ESP data-ready BCM GPIO (SPI: 27; SPI-HD: 12)
  max_frequency=N       spi-max-frequency in Hz (default: 30000000)
  spi_cs_change=0|1     Optional full-duplex controller workaround (default: 0)

Legacy:
  resetpin=N            Alias for resetgpio=N when using SPI

Examples:
  ./rpi_init.sh sdio
  ./rpi_init.sh spi
  ./rpi_init.sh spi_hd resetgpio=6 datareadygpio=12
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
            spi|spi_hd|sdio|usb)
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
                SPI_DATAREADYGPIO_SET="1"
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

# The existing full-duplex mating-board default is BCM27. QIO needs a
# separate Data Ready line because SPI0 consumes the extra quad pins; use
# BCM12 by default for SPI-HD unless the caller explicitly overrides it.
if [ "$IF_TYPE" = "spi_hd" ] && [ "$SPI_DATAREADYGPIO_SET" = "0" ]; then
    SPI_DATAREADYGPIO="12"
fi

run_root modprobe bluetooth
run_root modprobe cfg80211

wlan_init

if [ "$BT_INIT_SET" != "0" ]; then
    bt_init
fi
