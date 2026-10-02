# OTA firmware update

ESP-Hosted-Linux can send an ESP firmware image from Linux to ESP over the hosted command path.

OTA is requested with the host driver's `ota_file` module parameter. Build the matching SDIO, SPI, or USB module first, then load it with the firmware image path.

SDIO example:

```sh
cd host
sudo insmod ./esp32_sdio.ko ota_file=/path/to/firmware.bin
```

SPI example:

```sh
cd host
sudo insmod ./esp32_spi.ko ota_file=/path/to/firmware.bin
```

USB example:

```sh
cd host
sudo insmod ./esp32_usb.ko ota_file=/path/to/firmware.bin
```

ESP-Hosted drivers do not use a `resetpin=` argument. For SPI, reset and sideband signals are configured in Device Tree; SDIO and USB use in-band control.

If the ESP-Hosted module is already loaded, unload it before reloading with `ota_file=`. After the hosted link initializes, the host reads the image and sends it to ESP. ESP writes the OTA image and restarts; the hosted link must come back after that restart.

## Before updating

- use an image built for the exact ESP target and expected partition layout
- keep host `dmesg` and ESP serial logs open
- use stable power during the update
- do not interrupt the transfer or ESP flash write

## After updating

Check the ESP firmware version/log after restart and confirm the Linux WLAN/HCI interfaces return.

If the update fails, identify which stage failed: opening the image, sending data, writing ESP flash, booting the new image, or re-establishing the hosted link.

[Build, flash, and load](../getting-started/build-and-flash.md) covers generic host-module build and loading.
