# Supported hardware

Choose an ESP target and the transport that connects it to the Linux host. The host platform must provide the selected bus plus the required GPIO, pinctrl, and kernel integration.

## ESP targets

| ESP target | Wi-Fi | Hosted transport | Bluetooth | IEEE 802.15.4 / Thread |
|---|---|---|---|---|
| ESP32 | 2.4 GHz, 802.11b/g/n | SDIO, SPI | BR/EDR + BLE | — |
| ESP32-S2 | 2.4 GHz, 802.11b/g/n | SPI | — | — |
| ESP32-S3 | 2.4 GHz, 802.11b/g/n | SPI | BLE | — |
| ESP32-S31 | 2.4 GHz, 802.11b/g/n | SPI, USB | BR/EDR + BLE | Hosted IEEE 802.15.4 Spinel RCP; included in S31 defaults |
| ESP32-C2 | 2.4 GHz, 802.11b/g/n | SPI | BLE | — |
| ESP32-C3 | 2.4 GHz, 802.11b/g/n | SPI | BLE | — |
| ESP32-C5 | 2.4 / 5 GHz, 802.11a/b/g/n/ac/ax | SDIO, SPI | BLE | — |
| ESP32-C6 | 2.4 GHz, 802.11b/g/n/ax | SDIO, SPI | BLE | Hosted IEEE 802.15.4 Spinel RCP; firmware opt-in |
| ESP32-C61 | 2.4 GHz, 802.11b/g/n/ax | SDIO, SPI | BLE | — |

The selected hosted transport always carries Wi-Fi data and control. When firmware advertises runtime secondary-radio control, hosted Bluetooth and IEEE 802.15.4 are selected with `radio_service`:

| Value | Secondary services |
|---|---|
| `none` | none |
| `bt` | Bluetooth HCI |
| `154` | IEEE 802.15.4 RCP |
| `bt+154` | Bluetooth HCI and IEEE 802.15.4 RCP |

Wi-Fi is not part of this mask.

## Choose SDIO, SPI, or USB

| | SDIO | SPI | USB |
|---|---|---|---|
| **Typical use** | Dedicated MMC/SDIO connection when both host and ESP expose SDIO | Broad target/host coverage; requires Handshake and Data Ready GPIOs | ESP32-S31 USB 2.0 High-Speed connection with no sideband GPIOs |
| **ESP targets** | ESP32, ESP32-C5, ESP32-C6, ESP32-C61 | ESP32, ESP32-S2, ESP32-S3, ESP32-S31, ESP32-C2, ESP32-C3, ESP32-C5, ESP32-C6, ESP32-C61 | ESP32-S31 |
| **Bus signals** | CLK, CMD, DAT0-DAT3 | SCLK, MOSI, MISO, CS | USB D+, USB D- (High Speed) |
| **Additional required GPIOs** | None for the runtime driver after enumeration; a platform MMC power sequence may be required by the board | ESP reset, Handshake, Data Ready | None |
| **Host considerations** | SDIO controller, pull-ups, routing and signal integrity | SPI controller plus Device Tree node for reset, Handshake, and Data Ready | Standard USB host controller and USB connection |

> **SPI requires Handshake and Data Ready.** These GPIOs are part of normal transport operation. Configure them in ESP firmware and Linux Device Tree and connect both signals physically.

SPI also uses a host-controlled ESP reset/enable connection. SDIO recovery is in-band after the function has enumerated; a board may still need MMC power sequencing for initial enumeration. USB reset/recovery uses USB control requests and does not need sideband GPIOs.

## Wi-Fi

Wi-Fi is exposed as a normal Linux `wlanX` interface using `cfg80211`/`nl80211`. Station mode uses standard Linux tools such as `wpa_supplicant`; AP mode uses `hostapd` and requires the host module to be built with AP support.

ESP32-C5 is the only target in this table with both 2.4 GHz and 5 GHz Wi-Fi. The other listed targets use 2.4 GHz.

See [Wi-Fi station](../guides/wifi-station.md) and [Wi-Fi access point](../guides/wifi-access-point.md).

## Bluetooth

On Bluetooth-capable targets, HCI can share the hosted SDIO/SPI/USB transport. Supported setups can instead use HCI over UART.

ESP32 and ESP32-S31 expose BR/EDR + BLE dual-mode HCI. The other Bluetooth-capable targets in the table expose BLE HCI.

Hosted HCI is activated with `radio_service=bt` or `radio_service=bt+154`. UART HCI is a separate physical interface and is outside the hosted radio-service selector.

See [Bluetooth](../guides/bluetooth.md) and [Hardware setup](../getting-started/hardware-setup.md#bluetooth-hci-over-uart).

## IEEE 802.15.4 RCP (Thread and Zigbee)

ESP-Hosted-Linux exposes the ESP IEEE 802.15.4 radio through a Spinel/HDLC byte stream at `/dev/esp_rcp0`. The firmware uses the ESP-IDF OpenThread RCP backend. Compatible Thread and Zigbee Radio Spinel host stacks can use this interface.

Current physical qualification in this repository covers OpenThread:

| Target | Transport | Qualification |
|---|---|---|
| ESP32-C6 | SPI | OpenThread Spinel + real Thread RF |
| ESP32-C6 | SDIO | OpenThread Spinel + real Thread RF |
| ESP32-S31 | USB | OpenThread Spinel + real Thread RF |

ESP32-S31 includes the RCP support in its default firmware configuration; the service is still activated at runtime with `radio_service=154` or `bt+154`. ESP32-C6 requires the RCP/OpenThread firmware options to be enabled explicitly.

Zigbee uses the same Radio Spinel RCP model, but this repository does not bundle or HIL-qualify a Linux Zigbee userspace stack.

See [IEEE 802.15.4 RCP (Thread and Zigbee)](../guides/thread-rcp.md).

## Host requirements

A Linux host needs:

- a working SDIO, SPI, or USB controller for the selected transport
- for SPI, Device Tree support describing the `espressif,esp32-spi` node with reset, Handshake, and Data Ready GPIOs
- for SDIO, an MMC/SDIO controller with appropriate pinctrl and optional `mmc-pwrseq` if pre-enumeration board reset is required
- kernel headers or a prepared kernel tree matching the running kernel

[Porting](../porting.md) covers integration with another Linux platform.

## Power management

Host sleep and ESP wakeup are currently supported on the SDIO path. The Linux platform needs a wake-capable GPIO and suspend/resume integration in addition to the normal SDIO wiring.

See [Host sleep](../guides/host-sleep.md).

## Kernel compatibility

Host-driver source compatibility currently covers Linux 4.9 through 7.2. Kernel version alone does not guarantee platform support; the selected bus controller, GPIO/pinctrl integration, and required kernel subsystems must also be available.
