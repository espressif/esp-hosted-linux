# Build, flash, and load

ESP-Hosted-Linux has two build products:

1. ESP firmware in `esp/esp_driver/network_adapter/`
2. Linux host driver in `host/`

The ESP firmware flow is independent of the Linux host. The host driver can run on any Linux platform with the required SDIO/SPI/USB, GPIO, and kernel integration.

## 1. Prepare ESP-IDF

The repository pins the ESP-IDF revision used by its firmware.

On Linux or macOS:

```sh
cd esp/esp_driver
./setup.sh
. ./esp-idf/export.sh
```

A normal `./setup.sh` does not reset the main repository. It clones the expected ESP-IDF revision when needed and installs its prerequisites.

<details markdown="1">
<summary><strong>If an existing esp-idf checkout is at the wrong revision</strong></summary>

The setup script stops rather than changing an unexpected checkout automatically.

- `./setup.sh -u` resets and updates only the managed `esp-idf/` checkout after confirmation.
- `./setup.sh -f` is destructive: after confirmation it runs `git reset --hard` in the main repository, removes `esp-idf/`, and recreates it.

Do not use `-f` when you have local work to keep.

</details>

<details markdown="1">
<summary><strong>Windows ESP firmware setup</strong></summary>

From PowerShell:

```powershell
cd esp\esp_driver
.\setup.ps1
.\esp-idf\export.ps1
cd network_adapter
```

Then use the same `idf.py set-target`, `menuconfig`, `build`, and `flash` flow described below.

</details>

## 2. Build and flash ESP firmware

```sh
cd network_adapter
idf.py set-target ESP_TARGET
idf.py menuconfig
```

Replace `ESP_TARGET` with the selected target, such as `esp32c6` (use `idf.py --preview set-target esp32s31` for ESP32-S31 in ESP-IDF v6.1).

Under **Example Configuration → Transport layer**, choose **SDIO interface**, **SPI interface**, or **USB interface**. SDIO appears only when the target has SDIO-slave support, and USB appears on ESP32-S31.

For SPI, keep the configured **Handshake** and **Data Ready** GPIOs consistent with the Linux host configuration and physical wiring.

Build and flash:

```sh
idf.py -p SERIAL_PORT build flash
```

Replace `SERIAL_PORT` with the ESP console/flash port used on your development system.

Open the serial monitor:

```sh
idf.py -p SERIAL_PORT monitor
```

Use [Supported hardware](../reference/supported-hardware.md) to choose a target and transport, and [Hardware setup](hardware-setup.md) for signal requirements.

## 3. Build the Linux host driver

Build against the kernel that will load the module. For a native build, the normal kernel build tree is:

```text
/lib/modules/$(uname -r)/build
```

Install matching kernel headers or prepare the kernel build tree first.

From `host/`:

```sh
make target=sdio
# or
make target=spi
# or
make target=usb
```

For cross-builds, pass `ARCH`, `CROSS_COMPILE`, and `KERNEL`:

```sh
make target=sdio \
  ARCH=TARGET_ARCH \
  CROSS_COMPILE=/path/to/toolchain-prefix- \
  KERNEL=/path/to/kernel/build
```

Use `target=spi` for SPI or `target=usb` for USB. [Porting](../porting.md) covers bus, GPIO, pinctrl, and device-tree integration on another Linux platform.

## 4. Load the Linux driver

ESP-Hosted kernel drivers do not take a `resetpin=` module argument. Reset and control GPIOs are owned by Device Tree or handled in-band:

- **SDIO:** Post-enumeration reset and recovery are handled in-band over the SDIO bus. Insert the module directly:

```sh
sudo modprobe bluetooth
sudo modprobe cfg80211
sudo insmod ./esp32_sdio.ko
```

- **SPI:** Reset, Handshake, and Data Ready GPIOs are described in Device Tree (`reset-gpios`, `handshake-gpios`, `data-ready-gpios`). The driver binds to the `espressif,esp32-spi` node:

```sh
sudo modprobe bluetooth
sudo modprobe cfg80211
sudo insmod ./esp32_spi.ko
```

On Raspberry Pi, `./rpi_init.sh spi` compiles and applies the Device Tree overlay (`overlays/esp32-spi.dtbo` and `spidev_disabler.dtbo`) automatically. See [Raspberry Pi reference](../reference/raspberry-pi.md) and [SPI Device Tree integration](../architecture/spi.md#device-tree-integration).

- **USB:** Reset and recovery are handled via in-band USB vendor control requests:

```sh
sudo modprobe bluetooth
sudo modprobe cfg80211
sudo insmod ./esp32_usb.ko
```

### Select Bluetooth and IEEE 802.15.4 services

Wi-Fi does not require a secondary-radio selection. Hosted Bluetooth and the IEEE 802.15.4 RCP are selected with `radio_service` when firmware advertises runtime radio-service control:

| Value | Result |
|---|---|
| `none` | Wi-Fi only |
| `bt` | Wi-Fi + hosted Bluetooth HCI |
| `154` | Wi-Fi + IEEE 802.15.4 RCP |
| `bt+154` | Wi-Fi + hosted Bluetooth HCI + IEEE 802.15.4 RCP |

Set the value at module insertion:

```sh
sudo insmod ./esp32_usb.ko radio_service=154
```

or change it later:

```sh
echo bt+154 | sudo tee /sys/module/esp32_usb/parameters/radio_service
cat /sys/module/esp32_usb/parameters/radio_service_active
```

Use `esp32_sdio` or `esp32_spi` in the sysfs path for those transports. Adding a service is applied in place. Removing any active hosted secondary service requires a controlled ESP firmware restart.

Unload with:

```sh
sudo rmmod esp32_sdio
# or
sudo rmmod esp32_spi
# or
sudo rmmod esp32_usb
```

## 5. Verify bring-up

```sh
lsmod | grep esp32
iw dev
ip link
```

A successful Wi-Fi bring-up creates a `wlanX` interface. Do not assume it will always be `wlan0`.

If the interface is missing, inspect the kernel log:

```sh
dmesg | tail -n 100
```

Then continue with [Troubleshooting](../troubleshooting.md).
