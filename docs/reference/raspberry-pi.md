# Raspberry Pi reference setup

Raspberry Pi is a convenient development and validation host for ESP-Hosted-Linux, but it is not required. Use the generic [Hardware setup](../getting-started/hardware-setup.md), [Build, flash, and load](../getting-started/build-and-flash.md), and [Porting](../porting.md) pages for the platform-independent flow.

This page collects the Raspberry Pi-specific pin mappings, boot configuration, UART setup, and `rpi_init.sh` helper usage in one place.

## Pin numbering

Pin numbers in the tables below are **physical 40-pin header numbers**. The `resetpin=` argument passed to the Linux module uses a **BCM GPIO number**. The project helper defaults to BCM6, which is physical pin 31.

Raspberry Pi GPIO is 3.3 V logic. Do not connect ESP signals to 5 V logic.

## SPI reference wiring

Use the ESP-side signal assignments from [Hardware setup](../getting-started/hardware-setup.md#spi). On Raspberry Pi, the reference host mapping is:

| Function | Raspberry Pi physical pin |
|---|---:|
| CS0 | 24 |
| SCLK | 23 |
| MISO | 21 |
| MOSI | 19 |
| Ground | 25 |
| Handshake (ESP → host) | 15 |
| Data Ready (ESP → host) | 13 |
| ESP reset | 31 |

**Handshake and Data Ready are mandatory for SPI operation.** The helper uses BCM22 for Handshake, BCM27 for Data Ready, and BCM6 for ESP reset by default.

Enable SPI in the active Raspberry Pi `config.txt`:

```text
dtparam=spi=on
```

Recent Raspberry Pi OS images keep this file at `/boot/firmware/config.txt`; older images may use `/boot/config.txt`. Reboot after changing it.

The project helper builds and loads `spidev_disabler.dts` before inserting the ESP SPI module so `spidev` does not claim the same chip select.

## SDIO reference wiring

Use the ESP-side signal assignments and pull-up guidance from [Hardware setup](../getting-started/hardware-setup.md#sdio). On Raspberry Pi, the reference mapping is:

| Function | Raspberry Pi physical pin |
|---|---:|
| DAT3 | 13 |
| CLK | 15 |
| CMD | 16 |
| DAT0 | 18 |
| DAT1 | 22 |
| DAT2 | 37 |
| ESP reset | 31 |
| Ground | 39 |

Enable the SDIO overlay in the active Raspberry Pi `config.txt`:

```text
dtoverlay=sdio,poll_once=off
```

Reboot after changing it.

## Bluetooth HCI over UART

If Bluetooth HCI uses UART, connect the Raspberry Pi UART to the ESP-side pins listed in [Hardware setup](../getting-started/hardware-setup.md#bluetooth-hci-over-uart).

| Signal on Raspberry Pi | Physical pin | Connect to ESP |
|---|---:|---|
| TX | 8 | ESP RX |
| RX | 10 | ESP TX |
| RTS | 11 | ESP CTS |
| CTS | 36 | ESP RTS |

Enable the UART hardware and free the onboard Bluetooth UART when the 40-pin UART is used for ESP HCI:

```text
enable_uart=1
dtoverlay=disable-bt
```

Disable the Raspberry Pi Bluetooth UART service:

```sh
sudo systemctl disable hciuart
```

Remove any `console=serial0,...` setting from the active kernel command line before using that UART for HCI. Recent Raspberry Pi OS images keep the command line in `/boot/firmware/cmdline.txt`.

Raspberry Pi 5 has different default UART routing from earlier boards. Check the active `/dev/serial0` mapping before assuming it points to physical pins 8 and 10.

## `rpi_init.sh` helper

From `host/`:

```sh
./rpi_init.sh sdio
# or
./rpi_init.sh spi
```

The helper combines the common module build/load flow with Raspberry Pi-specific bus and GPIO setup. It uses BCM6 for ESP reset unless overridden.

Useful arguments include:

| Argument | Purpose |
|---|---|
| `ap_support` | Build with access-point support |
| `btuart` | Configure Raspberry Pi pins for 4-wire HCI UART |
| `btuart_2pins` | Configure Raspberry Pi TX/RX pins for HCI UART without hardware flow control |
| `resetpin=<BCM-number>` | Override the ESP reset GPIO; default is BCM6 |
| `rawtp_host_to_esp` | Run raw host-to-ESP transport traffic |
| `rawtp_esp_to_host` | Run raw ESP-to-host transport traffic |
| `ota_file=/path/to/firmware.bin` | Send a firmware image to ESP |

Run `./rpi_init.sh --help` for the complete argument list.

## Verify bring-up

After loading the driver:

```sh
iw dev
ip link
lsmod | grep esp32
```

If the ESP interface does not appear, save `dmesg` and the ESP serial log and continue with [Troubleshooting](../troubleshooting.md).
