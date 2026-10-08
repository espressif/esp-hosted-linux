# ESP-Hosted-Linux documentation

New to ESP-Hosted-Linux? Start with [Quick start](getting-started/quick-start.md). It covers firmware setup, hardware requirements, host integration, driver loading, and first bring-up.

## Start here

- [Quick start](getting-started/quick-start.md) — build firmware, connect hardware, load the driver, and verify `wlanX`
- [Supported hardware](reference/supported-hardware.md) — ESP targets, Wi-Fi capabilities, transport choices, Bluetooth, and 802.15.4 RCP support
- [Hardware setup](getting-started/hardware-setup.md) — generic SDIO, SPI, USB, reset, pull-up, and optional Bluetooth UART requirements
- [Build, flash, and load](getting-started/build-and-flash.md) — ESP-IDF setup, firmware build, flashing, and Linux host-driver loading

## Wireless guides

- [Wi-Fi station](guides/wifi-station.md) — scan, connect with `wpa_supplicant`, get an IP address, and debug association
- [Wi-Fi access point](guides/wifi-access-point.md) — run `hostapd`, configure WPA2/WPA3, and add local DHCP
- [Bluetooth](guides/bluetooth.md) — use hosted HCI or HCI-over-UART with BlueZ
- [IEEE 802.15.4 RCP (Thread / Zigbee)](guides/thread-rcp.md) — use the hosted Spinel/HDLC RCP through `/dev/esp_rcp0`; OpenThread is qualified end-to-end, and compatible Zigbee Radio Spinel stacks use the same RCP protocol
- [Host sleep](guides/host-sleep.md) — suspend a Linux host and wake it from ESP over the SDIO host-wakeup path
- [OTA firmware update](guides/ota.md) — send a firmware image from Linux to ESP over the hosted link

## Architecture

- [Overview](architecture/overview.md) — Linux control/data paths, Bluetooth HCI, ESP firmware, and the common transport header
- [SDIO transport](architecture/sdio.md) — function IDs, registers, counters, CMD53 flow, and bring-up order
- [SPI transport](architecture/spi.md) — required sideband GPIOs, transfer flow, initial clocking, and chip-select timing
- [USB transport](architecture/usb.md) — USB 2.0 High-Speed bulk interface, framing, preallocated URBs, and in-band reset recovery

## Reference and platform work

- [Supported hardware](reference/supported-hardware.md) — target and transport reference
- [Performance](reference/performance.md) — Wi-Fi throughput measurements and raw-transport test mode
- [Repository layout](reference/repository-layout.md) — where host, firmware, docs, and tools live
- [Porting](porting.md) — integrate ESP-Hosted-Linux with another Linux platform
- [Troubleshooting](troubleshooting.md) — debug transport bring-up, Wi-Fi, Bluetooth, and Thread/RCP failures
- [Raspberry Pi reference setup](reference/raspberry-pi.md) — optional lab/reference pin mappings, boot configuration, and helper script
- [Migration](migration.md) — move work from the old `esp_hosted_ng/` tree
