# ESP-Hosted SPI Device Tree example

The SPI host driver binds to a firmware-described SPI device instead of creating
`spi0.0` from hard-coded bus, chip-select, and GPIO numbers. The SPI node is
the production source of truth for:

- `compatible = "espressif,esp32-spi"`
- `spi-max-frequency`
- standard SPI mode properties `spi-cpol` / `spi-cpha` (the example is mode 2)
- `reset-gpios` (required)
- `handshake-gpios` (required)
- `data-ready-gpios` (required)

The Raspberry Pi example preserves the historical ESP-Hosted wiring defaults:
BCM GPIO6 for reset, GPIO22 for handshake, and GPIO27 for data-ready. The ESP SPI chip select remains electrically active-low; the overlay therefore does not request `spi-cs-high`. Linux may still use its internal `SPI_CS_HIGH` representation for GPIO-backed CS handling, which is distinct from requesting active-high CS in Device Tree. The
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
deployment manager. The Raspberry Pi SDIO boot overlay is not removed, so it
can remain active when the SPI control GPIOs use different pins.

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
be three distinct GPIOs. An SDIO overlay using GPIO22..27 may remain active if
the selected SPI control GPIOs do not overlap those pins.

GPIO polarity is part of each GPIO descriptor. The example uses active-low
reset and active-high handshake/data-ready; the driver selects the matching
physical IRQ edge from descriptor polarity. The example also declares
`spi-cpol`, so the SPI core supplies mode 2 (CPOL=1, CPHA=0) to the driver.

`spi-max-frequency` is a board/controller safety cap. The driver starts at
its normal 10 MHz default unless `clockspeed` requests another rate, and
clamps the requested rate to both the Device Tree maximum and the SPI
protocol's 40 MHz maximum.

Unload `esp32_spi` before changing or removing a runtime overlay.

For compatibility with older Raspberry Pi helper invocations,
`rpi_init.sh spi resetpin=N` is accepted as an alias for `resetgpio=N`.
The kernel module itself no longer accepts `resetpin=`; SPI reset ownership is
described by `reset-gpios` in Device Tree. The helper's reset GPIO option has
no effect on SDIO, whose post-enumeration reset/recovery is in-band.


## Porting note

This overlay targets Raspberry Pi `spi0`/CS0 and the classic Raspberry Pi
`brcm,*` GPIO/pinctrl overlay symbols. It is **not a generic RP1/Pi 5 pinctrl
example**. On Raspberry Pi 5/RP1, describe the same ESP properties with the
platform's current RP1/generic pinctrl binding (preferably in boot/base DT) and
verify reset direction electrically before enabling runtime reset control.

Other hosts should likewise describe the ESP node under their own SPI
controller and use their platform's native pinctrl binding rather than copying
the Raspberry Pi `brcm,*` pinctrl properties.
