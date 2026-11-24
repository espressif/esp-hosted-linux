# ESP-Hosted SPI Device Tree example

The SPI host driver binds to a firmware-described SPI device instead of creating
`spi0.0` from hard-coded bus, chip-select, and GPIO numbers. The SPI node is
the production source of truth for:

- `compatible = "espressif,esp32-spi"`
- `spi-max-frequency`
- standard SPI mode properties `spi-cpol` / `spi-cpha` (the example is mode 2)
- `reset-gpios` (optional in the driver; present in this reference overlay)
- `handshake-gpios` (required)
- `data-ready-gpios` (required)

The Raspberry Pi example preserves the historical ESP-Hosted wiring defaults:
BCM GPIO6 for reset, GPIO22 for handshake, and GPIO27 for data-ready.
ESP-IDF SPI Slave expects standard active-low CS on the wire (idle high,
asserted low when the master clocks a transfer). ESP-Hosted firmware does not
invert CS in software, so these overlays do not request `spi-cs-high`. When an
overlay uses GPIO-backed CS, the `cs-gpios` entry carries the active-low
(inverted) GPIO flag. Linux may still use its internal `SPI_CS_HIGH`
representation for GPIO-backed CS handling, which is distinct from requesting
active-high CS in Device Tree. The
overlay requests input directions (with a pull-up on reset) as part of the
device pinctrl state. The driver acquires reset through the GPIO descriptor API
as an input and switches it to output only for the deliberate reset pulse after
all dependencies are available. The helper does not force GPIO directions from
userspace, so it cannot silently override another kernel driver's pin ownership.

## Raspberry Pi runtime setup

Prefer `./rpi_init.sh spi` for Raspberry Pi bring-up. The script builds the
requested transport first, unloads a currently loaded ESP-Hosted transport,
replaces only its runtime SPI overlays, loads the freshly built module, and
checks that `spi0.0` binds. It intentionally does not implement persistent
rollback/provenance tracking; it is a bring-up/test helper rather than a
deployment manager. Boot-time SDIO configuration is not removed dynamically:
the helper inspects the live Raspberry Pi SDIO pin group and ESP pwrseq reset
GPIO before enabling SPI. It refuses a real overlap with SPI0 or the selected
ESP control GPIOs, and fails closed when an active custom SDIO configuration
cannot be mapped safely.

For a manual runtime flow, disable SPI **binding** auto-probe during the
replacement window and preload the exact module before creating the ESP DT
child. Device creation still emits a modalias uevent even when
`drivers_autoprobe=0`, so preloading the wanted module is what prevents an
older installed `esp32_spi` from winning a udev/modprobe race:

```bash
set -e
dtc -@ -I dts -O dtb -o spidev_disabler.dtbo spidev_disabler.dts
dtc -@ -I dts -O dtb -o overlays/esp32-spi.dtbo overlays/esp32-spi.dts

old_autoprobe="$(cat /sys/bus/spi/drivers_autoprobe)"
restore_autoprobe()
{
    printf '%s\n' "$old_autoprobe" | sudo tee /sys/bus/spi/drivers_autoprobe >/dev/null
}
trap restore_autoprobe EXIT

printf '0\n' | sudo tee /sys/bus/spi/drivers_autoprobe >/dev/null
if [ -d /sys/module/esp32_spi ]; then
    sudo rmmod esp32_spi
fi
sudo insmod ./esp32_spi.ko

sudo dtoverlay -d . spidev_disabler
sudo dtoverlay -d overlays esp32-spi
grep esp32-spi /sys/bus/spi/devices/spi0.0/modalias
printf 'spi0.0\n' | sudo tee /sys/bus/spi/drivers_probe

restore_autoprobe
trap - EXIT
```

The Raspberry Pi SPI controller remains enabled. Only the conflicting default
`spidev0` child must be disabled in a **separate** overlay before the ESP node
is created. Combining both operations in one
runtime overlay can leave a stale SPI device holding chip-select 0 and fail
with `chipselect 0 already in use`.

Only runtime-applied overlays can be removed with `dtoverlay -r`. If the ESP
node is provided by `config.txt` or the base Device Tree, change that boot/base
description instead of expecting runtime GPIO overrides to replace it. For
normal bring-up, `rpi_init.sh spi` removes and reapplies its runtime
`esp32-spi` and `spidev_disabler` overlays using the GPIO/frequency values
supplied on the command line.

## Wiring overrides

The historical defaults are reset=BCM6, handshake=BCM22 and data-ready=BCM27.
Override them directly through the bring-up helper when the board is wired
differently:

```bash
./rpi_init.sh spi resetgpio=6 handshakegpio=5 datareadygpio=12
```

The equivalent direct overlay command is:

```bash
sudo dtoverlay -d overlays esp32-spi \
    resetgpio=6 handshakegpio=5 datareadygpio=12 max_frequency=30000000
```

For the classic Raspberry Pi runtime overlay, `rpi_init.sh` accepts BCM GPIO
numbers 0..53 but rejects GPIO7..GPIO11 because those pins are already consumed
by SPI0 chip-select/MISO/MOSI/SCLK. Reset, handshake, and data-ready must also
be three distinct GPIOs. The default SPI and SDIO mappings overlap (SDIO
GPIO22..27 versus SPI Handshake/Data Ready, plus BCM6 when the default SDIO
pwrseq and SPI reset are both used). Alternate SDIO pin groups can coexist with
SPI when the live SDIO data pins, pwrseq reset GPIO, SPI0 GPIO7..11, and the
selected ESP control GPIOs are all disjoint. The helper checks that live set
before applying the SPI overlay.

GPIO polarity is part of each GPIO descriptor. The example uses active-low
reset and active-high handshake/data-ready; the driver selects the matching
physical IRQ edge from descriptor polarity. The example also declares
`spi-cpol`, so the SPI core supplies mode 2 (CPOL=1, CPHA=0) to the driver.

`spi-max-frequency` is the board/controller safety cap. SPI bootstrap runs at
the safe low rate; after a valid boot event the driver normally accepts the
firmware-advertised operating clock, bounded by Device Tree and the protocol
maximum. The legacy `clockspeed` module parameter remains an initial/fallback
host rate, not a steady-state ceiling. If compatible firmware omits the optional
clock TLV, the driver restores `clockspeed` when nonzero or the historical
10 MHz default instead of remaining at the 1 MHz bootstrap rate.

Unload `esp32_spi` before changing or removing a runtime overlay.

For compatibility with older Raspberry Pi helper invocations,
`rpi_init.sh spi resetpin=N` is accepted as an alias for `resetgpio=N`.
The kernel module itself no longer accepts `resetpin=`; SPI reset ownership is
described by `reset-gpios` in Device Tree.

For SDIO, `esp32-sdio-pwrseq.dtbo` describes a boot-time
`mmc-pwrseq-simple` that controls ESP EN and marks the MMC host with
`espressif,full-chip-reset`. The driver trusts MMC hardware reset as a firmware
reincarnation only when both the pwrseq and this explicit marker are present.
Because MMC pwrseq is acquired when the host is probed, install/apply this
overlay in the boot configuration and reboot; do not rely on a late runtime
overlay to retrofit reset ownership. Without the marker the driver safely uses
the generation-ACKed in-band reboot fallback.


## Porting note

This overlay targets Raspberry Pi `spi0`/CS0 and the classic Raspberry Pi
`brcm,*` GPIO/pinctrl overlay symbols. It is **not a generic RP1/Pi 5 pinctrl
example**. On Raspberry Pi 5/RP1, describe the same ESP properties with the
platform's current RP1/generic pinctrl binding (preferably in boot/base DT) and
verify reset direction electrically before enabling runtime reset control.

Other hosts should likewise describe the ESP node under their own SPI
controller and use their platform's native pinctrl binding rather than copying
the Raspberry Pi `brcm,*` pinctrl properties.


## Raspberry Pi 5 SPI-HD/QIO

Use `esp32-spi-hd-rpi5.dts` with `compatible = "espressif,esp32-spi-hd"`.
The RP1 controller needs quad support; build the controller modules with
`bash rpi5_spi_hd/build-rp1-quad-modules.sh --load`. The QSPI transport uses
D0–D3, CLK, CS and Data Ready; it does not use the full-duplex handshake line.

See [QSPI setup and tested profile](../../docs/rpi5-spi-hd-qio.md) for wiring,
firmware configuration, runtime setup and qualification scope.
