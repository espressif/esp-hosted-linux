# Porting to another Linux host

ESP-Hosted-Linux is designed to integrate with Linux platforms that provide the required SDIO, SPI, or USB controller, Device Tree/pinctrl support, and kernel build environment.

## Porting checklist

1. Pick SDIO, SPI, or USB and make sure Linux exposes that controller.
2. For SPI, create an `espressif,esp32-spi` Device Tree node defining `reset-gpios`, `handshake-gpios`, and `data-ready-gpios`.
3. For SDIO, configure the MMC controller in Device Tree; add platform power-sequence reset (`mmc-pwrseq`) if required for enumeration.
4. For USB, connect to a standard USB host controller; no Device Tree description is needed.
5. Build the module against the target kernel tree.
6. Check basic bus operation and enumeration before debugging Wi-Fi.
7. Start with a conservative bus clock.
8. Confirm the ESP boot/capability exchange.
9. Raise performance settings only after transfers are stable.

## Power and wiring

Use a stable power supply. Marginal host or ESP power often looks like random transport failure.

For SPI, provide the chip-select pull-up plus the required Handshake and Data Ready GPIOs.

For SDIO, follow CMD/DAT pull-up requirements for the ESP target. Avoid long jumper wires; use a controlled interconnect for high-speed operation.

[Hardware setup](getting-started/hardware-setup.md) shows the required signal sets and ESP-side pin assignments.

## Device tree and pinctrl

ESP-Hosted kernel drivers do not use hardcoded GPIOs or module arguments (`resetpin=` has been removed from all modules). All hardware signal mappings and pin configurations are owned by the platform's Device Tree.

### SPI Device Tree binding

The SPI driver binds to a device tree node with `compatible = "espressif,esp32-spi"` located under the host SPI controller node.

Required and optional properties:

- `compatible`: Must be `"espressif,esp32-spi"`.
- `reg`: The chip-select index on the SPI bus.
- `spi-max-frequency`: The maximum bus clock rate in Hz (capped at 40 MHz by protocol).
- `spi-cpol`: Present to select SPI mode 2 (CPOL=1, CPHA=0).
- `reset-gpios`: Output GPIO descriptor connected to ESP reset/enable (active-low).
- `handshake-gpios`: Interrupt-capable input GPIO descriptor for ESP Handshake (active-high).
- `data-ready-gpios`: Interrupt-capable input GPIO descriptor for ESP Data Ready (active-high).

Example Device Tree snippet:

```dts
&spi1 {
    status = "okay";
    #address-cells = <1>;
    #size-cells = <0>;

    esp32_spi: esp32-spi@0 {
        compatible = "espressif,esp32-spi";
        reg = <0>;
        spi-max-frequency = <30000000>;
        spi-cpol;
        pinctrl-names = "default";
        pinctrl-0 = <&esp_spi_pins>;
        reset-gpios = <&pio 1 10 GPIO_ACTIVE_LOW>;
        handshake-gpios = <&pio 1 11 GPIO_ACTIVE_HIGH>;
        data-ready-gpios = <&pio 1 12 GPIO_ACTIVE_HIGH>;
    };
};
```

Ensure the pin controller (pinctrl) configures the SCLK, MOSI, MISO, and CS lines appropriately, and that `spidev` or another driver does not claim the same chip select.

### SDIO platform integration

SDIO recovery and link reset are handled entirely in-band after the SDIO function has enumerated.

Map CLK, CMD, and DAT0-DAT3 to an MMC/SDIO controller in the platform Device Tree:

- Add `non-removable` for hardwired modules.
- Add `cap-sd-highspeed` if supported.
- If the board requires host GPIO control to cycle power or toggle ESP reset before the SDIO card can enumerate, attach an `mmc-pwrseq-simple` node to the MMC controller:

```dts
wifi_pwrseq: wifi-pwrseq {
    compatible = "mmc-pwrseq-simple";
    reset-gpios = <&gpio 6 GPIO_ACTIVE_LOW>;
    post-power-on-delay-ms = <50>;
};

&mmc1 {
    status = "okay";
    non-removable;
    bus-width = <4>;
    mmc-pwrseq = <&wifi_pwrseq>;
};
```

### USB integration

The USB transport uses standard Linux USB subsystem enumeration. When ESP32-S31 is connected to any standard USB host port, Linux detects device `303a:4002` and the `esp32_usb` driver binds directly. No Device Tree or platform GPIO configuration is required.

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
