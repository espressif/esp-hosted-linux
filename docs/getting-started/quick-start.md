# Quick start

This walkthrough is platform-neutral. ESP-Hosted-Linux needs a Linux host with the selected SDIO or SPI controller, required GPIOs, and a kernel build environment. Platform-specific bus, pinctrl, and device-tree integration is covered in [Porting](../porting.md).

## Before you start

You need:

- a supported ESP target and SDIO or SPI transport
- a Linux host with a compiler, `make`, and headers/build tree for its running kernel
- transport wiring plus a host-controlled ESP reset signal
- for SPI, assigned and connected **Handshake** and **Data Ready** GPIOs
- a serial connection for ESP flashing and logs
- Git and the tools required by ESP-IDF
- `iw` for Wi-Fi bring-up; BlueZ tools if you plan to use Bluetooth

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

Replace `ESP_TARGET` with the selected target, such as `esp32c6`.

Under **Example Configuration → Transport layer**, choose SDIO or SPI. SDIO appears only on targets with an SDIO-slave peripheral.

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

Use [Hardware setup](hardware-setup.md) for signal requirements and [Porting](../porting.md) for platform integration.

## 3. Build and load the host driver

From `host/`:

```sh
make target=sdio
# or
make target=spi
```

Load the required Linux subsystems and the matching ESP-Hosted module:

```sh
sudo modprobe bluetooth
sudo modprobe cfg80211
sudo insmod ./esp32_sdio.ko resetpin=GPIO_NUMBER
```

Use `esp32_spi.ko` for SPI. Replace `GPIO_NUMBER` with the Linux GPIO connected to ESP reset/enable.

## 4. Check bring-up

List wireless interfaces instead of assuming the ESP interface is always `wlan0`:

```sh
iw dev
ip link
```

You should see a `wlanX` interface created by ESP-Hosted-Linux. Check module state and kernel logs if it is missing:

```sh
lsmod | grep esp32
dmesg | tail -n 100
```

If the ESP boot event never reaches Linux, start with [Troubleshooting](../troubleshooting.md).

## 5. Use Wi-Fi or Bluetooth

- [Wi-Fi station](../guides/wifi-station.md)
- [Wi-Fi access point](../guides/wifi-access-point.md)
- [Bluetooth](../guides/bluetooth.md)
- [OTA firmware update](../guides/ota.md)
