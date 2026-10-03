# Quick start

This walkthrough is platform-neutral. ESP-Hosted-Linux needs a Linux host with the selected SDIO, SPI, or USB controller, required GPIOs, and a kernel build environment. Platform-specific bus, pinctrl, and device-tree integration is covered in [Porting](../porting.md).

## Before you start

You need:

- a supported ESP target and SDIO, SPI, or USB transport
- a Linux host with a compiler, `make`, and headers/build tree for its running kernel
- for SDIO, the bus wiring and any platform-specific power-sequence/reset signal required before enumeration
- for SPI, assigned and connected **Handshake** and **Data Ready** GPIOs
- for USB, a standard USB connection between host and ESP
- a serial connection for ESP flashing and logs
- Git and the tools required by ESP-IDF
- `iw` for Wi-Fi bring-up, BlueZ tools for Bluetooth, and OpenThread POSIX (`ot-daemon` and `ot-ctl`) for Thread RCP bring-up

Pick the target and transport from [Supported hardware](../reference/supported-hardware.md), then follow [Hardware setup](hardware-setup.md).

## 1. Build and flash ESP firmware

Prepare the ESP-IDF checkout used by this repository:

```sh
cd esp/esp_driver
./setup.sh
. ./esp-idf/export.sh
cd network_adapter
```

Choose the ESP target and transport:

```sh
idf.py set-target ESP_TARGET
idf.py menuconfig
```

Replace `ESP_TARGET` with the selected target, such as `esp32c6` (use `idf.py --preview set-target esp32s31` for ESP32-S31).

Under **Example Configuration → Transport layer**, choose SDIO, SPI, or USB. SDIO appears only on targets with an SDIO-slave peripheral, and USB appears on targets with native USB-slave support (such as ESP32-S31).

For SPI, keep the configured Handshake and Data Ready GPIOs consistent with the Linux host integration and physical wiring.

Build, flash, and open the serial monitor:

```sh
idf.py -p SERIAL_PORT build flash monitor
```

Replace `SERIAL_PORT` with the ESP console/flash port used on your development system.

[Build, flash, and load](build-and-flash.md) covers setup options, Windows firmware setup, cross-builds, and module loading in more detail.

## 2. Prepare the Linux host

Configure the selected bus, pinctrl, reset GPIO, and any required sideband GPIOs before loading ESP-Hosted-Linux.

- **SDIO:** Linux must enumerate the ESP SDIO function, with the required CMD/DAT pull-ups and interrupt path working.
- **SPI:** Linux must own the intended SPI bus/chip select and receive Handshake and Data Ready GPIO interrupts.
- **USB:** Connect the target's USB port to the Linux host; standard Linux USB host drivers will enumerate the device.

Use [Hardware setup](hardware-setup.md) for signal requirements and [Porting](../porting.md) for platform integration.

## 3. Build and load the host driver

From `host/`:

```sh
make target=sdio
# or
make target=spi
# or
make target=usb
```

Load the required Linux subsystems and one ESP-Hosted transport module:

```sh
sudo modprobe bluetooth
sudo modprobe cfg80211

# Choose one:
sudo insmod ./esp32_sdio.ko
sudo insmod ./esp32_spi.ko
sudo insmod ./esp32_usb.ko
```

The commands above select the default secondary-radio service (`none`). If Bluetooth or Thread is needed immediately, add `radio_service=` to the one module command you use instead of inserting the module a second time.

ESP-Hosted kernel drivers do not take a `resetpin=` module argument. For SDIO and USB, reset and recovery are handled in-band. For SPI, reset, Handshake, and Data Ready GPIOs are defined in Device Tree under the `espressif,esp32-spi` node (see [SPI Device Tree](../architecture/spi.md#device-tree-integration) and [Raspberry Pi reference](../reference/raspberry-pi.md)).

Wi-Fi is independent of the secondary-radio selection. Current firmware keeps hosted Bluetooth and Thread off until requested with `radio_service`:

| Value | Hosted secondary services |
|---|---|
| `none` | none (Wi-Fi only) |
| `bt` | Bluetooth HCI |
| `154` | IEEE 802.15.4 RCP |
| `bt+154` | Bluetooth HCI and IEEE 802.15.4 RCP |

For example, use this as the USB module-load command when both hosted Bluetooth and Thread are required:

```sh
sudo insmod ./esp32_usb.ko radio_service=bt+154
```

The same parameter is available on the SDIO and SPI modules when firmware advertises the requested service. On an already loaded module, change it through `/sys/module/<module>/parameters/radio_service`.

## 4. Check bring-up

List wireless interfaces instead of assuming the ESP interface is always `wlan0`:

```sh
iw dev
ip link
```

A successful Wi-Fi bring-up creates a `wlanX` interface. If `radio_service` includes `bt`, Linux also registers an HCI controller. If it includes `154`, the driver creates `/dev/esp_rcp0` after firmware confirms RCP activation.

For example, with USB:

```sh
cat /sys/module/esp32_usb/parameters/radio_service_active
ls -l /dev/esp_rcp0
bluetoothctl list
```

Check module state and kernel logs if an expected interface is missing:

```sh
lsmod | grep esp32
dmesg | tail -n 100
```

If the ESP boot event never reaches Linux, start with [Troubleshooting](../troubleshooting.md).

## 5. Use Wi-Fi, Bluetooth, or Thread

- [Wi-Fi station](../guides/wifi-station.md)
- [Wi-Fi access point](../guides/wifi-access-point.md)
- [Bluetooth](../guides/bluetooth.md)
- [IEEE 802.15.4 RCP (Thread / Zigbee)](../guides/thread-rcp.md)
- [OTA firmware update](../guides/ota.md)
