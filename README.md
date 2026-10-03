<p align="center">
  <img src="docs/assets/espressif-logo.svg" width="320" alt="Espressif">
</p>

# ESP-Hosted-Linux

ESP-Hosted-Linux runs Wi-Fi and Bluetooth on supported Espressif SoCs. On targets with an IEEE 802.15.4 radio, it can also expose an IEEE 802.15.4 Radio Co-Processor (RCP) to the host. The firmware backend uses ESP-IDF's OpenThread RCP and HDLC-framed Spinel; compatible Thread and Zigbee Radio Spinel host stacks can use the same RCP interface. Wi-Fi data and control use SDIO, SPI, or USB. Hosted Bluetooth HCI and RCP traffic can share that transport. Bluetooth HCI can use UART on supported targets, and ESP32-S31 can place the RCP on a dedicated UART.

Linux uses its normal interfaces: `cfg80211`/`nl80211` and a `wlanX` netdev for Wi-Fi, Linux HCI for Bluetooth, and `/dev/esp_rcp0` for the hosted OpenThread Spinel stream. Existing tools such as `wpa_supplicant`, `hostapd`, `iw`, BlueZ, and OpenThread POSIX (`ot-daemon`) are used on the host.

<p align="center">
  <img src="docs/assets/system-architecture.svg" width="820" alt="ESP-Hosted-Linux architecture">
</p>

## 🔑 Key capabilities

- Standard Linux WLAN integration: `cfg80211`/`nl80211` control with a normal `wlanX` data path
- Wi-Fi station and access-point modes through `wpa_supplicant` and `hostapd`
- Bluetooth HCI over SDIO, SPI, or USB, with optional HCI-over-UART
- IEEE 802.15.4 RCP byte-stream endpoint (`/dev/esp_rcp0`) over supported SDIO, SPI, or USB paths for compatible Thread or Zigbee Radio Spinel host stacks; hosted Bluetooth and the RCP are runtime-selectable secondary services
- SDIO, SPI, and USB transports between Linux and the ESP device
- SDIO host sleep and ESP wakeup on supported Linux platforms
- ESP firmware update from Linux over the hosted link
- ESP firmware based on ESP-IDF

## 📦 Transports and targets

| Transport | Supported ESP targets |
|---|---|
| **SDIO** | ESP32, ESP32-C5, ESP32-C6, ESP32-C61 |
| **SPI** | ESP32, ESP32-S2, ESP32-S3, ESP32-S31, ESP32-C2, ESP32-C3, ESP32-C5, ESP32-C6, ESP32-C61 |
| **USB** | ESP32-S31 |

Bluetooth and Wi-Fi PHY capabilities vary by target. [Supported hardware](docs/reference/supported-hardware.md) has the full target table and transport notes.

## 📊 Performance

Best recorded Wi-Fi throughput from available project measurements:

| ESP target | Transport | Peak TCP | Peak UDP |
|---|---|---:|---:|
| ESP32 | SDIO | 43.5 Mbps | 49.1 Mbps |
| ESP32-C3 | SPI | 15.8 Mbps | 17.1 Mbps |
| ESP32-C5 | SDIO | 63.3 Mbps | 97.8 Mbps |
| ESP32-C6 | SDIO | 55.6 Mbps | 90.4 Mbps |
| ESP32-C61 | SDIO | 42.6 Mbps | 64.0 Mbps |

Peak values are maxima from available measurements and may come from different Tx/Rx directions or radio configurations. [Performance](docs/reference/performance.md) has the full measurements and test notes.

## 🚀 Quick start

1. Pick an ESP target and transport from [Supported hardware](docs/reference/supported-hardware.md).
2. Connect the ESP device to the Linux host using [Hardware setup](docs/getting-started/hardware-setup.md).
3. Build and flash ESP firmware using [Build, flash, and load](docs/getting-started/build-and-flash.md).
4. Configure the host bus/Device Tree integration, build the matching Linux module, and load it against the running kernel.
5. Continue with [Wi-Fi station](docs/guides/wifi-station.md), [Wi-Fi access point](docs/guides/wifi-access-point.md), [Bluetooth](docs/guides/bluetooth.md), or [IEEE 802.15.4 RCP (Thread / Zigbee)](docs/guides/thread-rcp.md).

For the complete bring-up flow, follow [Quick start](docs/getting-started/quick-start.md). Platform integration details are in [Porting](docs/porting.md).

## 📚 Documentation

- **Getting started** — [Quick start](docs/getting-started/quick-start.md), [hardware setup](docs/getting-started/hardware-setup.md), [build and flash](docs/getting-started/build-and-flash.md)
- **Wireless guides** — [Station](docs/guides/wifi-station.md), [access point](docs/guides/wifi-access-point.md), [Bluetooth](docs/guides/bluetooth.md), [IEEE 802.15.4 RCP (Thread / Zigbee)](docs/guides/thread-rcp.md), [host sleep](docs/guides/host-sleep.md), [OTA](docs/guides/ota.md)
- **Architecture** — [Overview](docs/architecture/overview.md), [SDIO](docs/architecture/sdio.md), [SPI](docs/architecture/spi.md), [USB](docs/architecture/usb.md)
- **Reference** — [Supported hardware](docs/reference/supported-hardware.md), [performance](docs/reference/performance.md), [repository layout](docs/reference/repository-layout.md), [Raspberry Pi reference setup](docs/reference/raspberry-pi.md)
- **Platform work** — [Porting](docs/porting.md), [troubleshooting](docs/troubleshooting.md)
- **Migration** — [Move from `esp_hosted`](docs/migration.md), [repository origin](ORIGIN.md)

## 🤝 Contributing

Contributions are welcome through GitHub pull requests. [CONTRIBUTING.md](CONTRIBUTING.md) covers the development workflow, commit sign-off, testing, and review.

## 📄 License

This repository contains code under the [Apache License 2.0](LICENSES/Apache-2.0) and [GNU GPL v2 only](LICENSES/GPL-2.0). Check each source file's SPDX identifier for the license that applies to that file. [LICENSES/README.md](LICENSES/README.md) summarizes the project license layout.
