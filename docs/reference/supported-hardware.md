# Supported hardware

Choose an ESP target and the transport that connects it to the Linux host. The host platform must provide the selected bus plus the required GPIO, pinctrl, and kernel integration.

## ESP targets

| ESP target | Wi-Fi | Hosted transport | Bluetooth |
|---|---|---|---|
| ESP32 | 2.4 GHz, 802.11b/g/n | SDIO, SPI | BR/EDR + BLE |
| ESP32-S2 | 2.4 GHz, 802.11b/g/n | SPI | — |
| ESP32-S3 | 2.4 GHz, 802.11b/g/n | SPI | BLE |
| ESP32-S31 | 2.4 GHz, 802.11b/g/n | SPI, USB | BLE |
| ESP32-C2 | 2.4 GHz, 802.11b/g/n | SPI | BLE |
| ESP32-C3 | 2.4 GHz, 802.11b/g/n | SPI | BLE |
| ESP32-C5 | 2.4 / 5 GHz, 802.11a/b/g/n/ac/ax | SDIO, SPI | BLE |
| ESP32-C6 | 2.4 GHz, 802.11b/g/n/ax | SDIO, SPI | BLE |
| ESP32-C61 | 2.4 GHz, 802.11b/g/n/ax | SDIO, SPI | BLE |

The hosted transport carries Wi-Fi data and control. On Bluetooth-capable targets, Bluetooth HCI can share the SDIO/SPI/USB link or use UART on supported setups.

## Choose SDIO, SPI, or USB

| | SDIO | SPI | USB |
|---|---|---|---|
| **Best fit** | Higher bus throughput when both host and ESP expose SDIO | Widest ESP target coverage and simpler host availability | High-speed plug-and-play bulk transport without host GPIO/Device Tree setup |
| **ESP targets** | ESP32, ESP32-C5, ESP32-C6, ESP32-C61 | ESP32, ESP32-S2, ESP32-S3, ESP32-S31, ESP32-C2, ESP32-C3, ESP32-C5, ESP32-C6, ESP32-C61 | ESP32-S31 |
| **Bus signals** | CLK, CMD, DAT0-DAT3 | SCLK, MOSI, MISO, CS | USB D+, USB D- (High Speed) |
| **Additional required GPIOs** | None for driver (in-band reset/recovery; optional platform MMC pwrseq) | ESP reset, Handshake, Data Ready (configured via Device Tree) | None (in-band vendor control requests) |
| **Host considerations** | SDIO controller, pull-ups, routing and signal integrity | SPI controller plus Device Tree node describing reset, handshake, and data-ready GPIOs | Standard USB host controller and USB connection |

> **Important — SPI requires Handshake and Data Ready.** These GPIOs are mandatory for ESP-Hosted SPI operation. Assign them in the ESP firmware and Linux host Device Tree configuration, and physically connect both signals between the ESP and host. They are not optional debug or flow-control pins.

SPI uses a host-controlled ESP reset/enable connection defined in Device Tree. SDIO and USB use in-band protocol requests for reset and recovery. See [Hardware setup](../getting-started/hardware-setup.md) for signal requirements and ESP pin assignments.

## Bluetooth

On Bluetooth-capable targets, HCI can use the hosted SDIO/SPI/USB transport. Supported setups can instead use HCI over UART.

UART pin assignments and flow-control requirements vary by target. See [Bluetooth](../guides/bluetooth.md) and [Hardware setup](../getting-started/hardware-setup.md#bluetooth-hci-over-uart).

## Host requirements

A Linux host needs:

- a working SDIO, SPI, or USB controller for the selected transport
- for SPI, Device Tree support describing the `espressif,esp32-spi` node with reset, Handshake, and Data Ready GPIOs
- for SDIO, an MMC/SDIO controller with appropriate pinctrl and optional `mmc-pwrseq` if pre-enumeration board reset is required
- kernel headers or a prepared kernel tree matching the running kernel

[Porting](../porting.md) covers integration with a Linux SoC or board platform.

## Power management

Host sleep and ESP wakeup are currently supported on the SDIO path. The Linux platform needs a wake-capable GPIO and suspend/resume integration in addition to the normal SDIO wiring.

See [Host sleep](../guides/host-sleep.md) for the supported flow and target-specific defaults.

## Kernel compatibility

Host-driver source compatibility currently covers Linux 4.9 through 7.2. Kernel version alone does not guarantee platform support; the selected bus controller, GPIO/pinctrl integration, and required kernel subsystems must also be available.
