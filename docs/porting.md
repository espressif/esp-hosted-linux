# Porting to another Linux host

ESP-Hosted-Linux is designed to integrate with Linux platforms that provide the required SDIO or SPI controller, GPIOs, pinctrl, and kernel build environment.

## Porting checklist

1. Pick SDIO or SPI and make sure Linux exposes that controller.
2. Map ESP signals in pinctrl/device tree.
3. Connect an ESP reset/enable GPIO.
4. Build the module against the target kernel tree.
5. Check basic bus operation before debugging Wi-Fi.
6. Start with a conservative bus clock.
7. Confirm the ESP boot/capability exchange.
8. Raise performance settings only after transfers are stable.

## Power and wiring

Use a stable power supply. Marginal host or ESP power often looks like random transport failure.

For SPI, provide the chip-select pull-up plus the required Handshake and Data Ready GPIOs.

For SDIO, follow CMD/DAT pull-up requirements for the ESP target. Avoid long jumper wires; use a controlled interconnect for high-speed operation.

[Hardware setup](getting-started/hardware-setup.md) shows the required signal sets and ESP-side pin assignments.

## Device tree and pinctrl

Linux must expose the chosen controller and GPIOs before ESP-Hosted can work.

### Reset

Connect an output GPIO to ESP reset/enable. The host module accepts its Linux GPIO number through the `resetpin` module parameter.

### SDIO

Map CLK, CMD, and DAT0-DAT3 to an MMC/SDIO controller.

Soldered or always-present devices often need properties such as `non-removable` or controller-specific card-detect handling. Use the bindings for the selected controller and board platform.

Make sure the SDIO function enumerates before debugging ESP-Hosted commands.

### SPI

Map SCLK, MOSI, MISO, CS, Handshake, and Data Ready. Handshake and Data Ready must be interrupt-capable host GPIO inputs.

ESP-side reference GPIO defaults are in:

```text
host/spi/esp_spi.h
```

Provide the SPI device through the normal device-tree or platform mechanism used by the target Linux system.

If `spidev` or another driver owns the same chip select, disable or unbind it before loading ESP-Hosted.

### Bluetooth HCI UART

If Bluetooth uses UART, configure TX/RX and optional CTS/RTS pinctrl. Remove any serial console using that UART. Linux and ESP must use the same baud rate and flow-control mode.

## Build for another kernel

`host/Makefile` accepts `ARCH`, `CROSS_COMPILE`, and `KERNEL`.

Example for arm64:

```sh
make -C host target=sdio \
  ARCH=arm64 \
  CROSS_COMPILE=/opt/toolchain/bin/aarch64-linux-gnu- \
  KERNEL=/path/to/kernel/build
```

Use `target=spi` for SPI.

The kernel build directory must be prepared for external modules and match the module ABI used on the target. [Supported hardware](reference/supported-hardware.md#kernel-compatibility) lists the source-compatibility range.

## ESP firmware settings

Choose the ESP transport under **Example Configuration → Transport layer**.

SPI options include Handshake/Data Ready GPIOs, queue sizes, checksum, controller choice where available, and `ESP_SPI_DEASSERT_HS_ON_CS`.

SDIO appears only on targets with SDIO-slave support. Checksum, speed, card-detect, and host-wakeup settings are also in project Kconfig.

Keep firmware settings and physical wiring in sync.

## SPI bring-up

Before raising clock speed:

- confirm the host GPIO resets ESP
- make sure Linux can claim the intended SPI bus and CS
- check Handshake and Data Ready interrupts
- confirm both sides use SPI mode 2 unless you deliberately changed both
- start at a low clock
- use short wiring
- repeat boot and data transfers until they are clean

The host starts at 10 MHz and can switch to the firmware-advertised SPI clock after boot information is received.

## SDIO bring-up

Check enumeration first:

```sh
ls /sys/bus/sdio/devices
```

Then inspect `dmesg` for probe, interrupt, or CMD53 errors.

If no SDIO function appears, fix power, pinmux, pull-ups, card-detect/controller setup, or wiring before changing ESP-Hosted command code.

[SDIO transport](architecture/sdio.md) describes the counters and register interface.

## Performance tuning

Once transport is stable:

- raise bus clock in steps
- watch for errors or retries
- test traffic in both directions
- test reset/reload cycles
- test suspend/resume if your product needs it
- record host, controller, kernel, and clock settings when comparing performance

## Porting notes

Keep board-specific hardware details out of common protocol code where possible. Device tree, normal kernel APIs, and platform resources are easier to maintain than growing target-specific `#ifdef` blocks. Protocol behavior should remain shared unless wire behavior actually differs.
