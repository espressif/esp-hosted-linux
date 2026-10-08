# ESP-Hosted-Linux firmware

This directory contains the ESP-IDF setup helpers used to build the ESP firmware in `network_adapter/`.

The canonical build instructions are in [Build, flash, and load](../../docs/getting-started/build-and-flash.md). Use [Supported hardware](../../docs/reference/supported-hardware.md) and [Hardware setup](../../docs/getting-started/hardware-setup.md) before selecting a target and transport.

## Linux and macOS

From `esp/esp_driver/`:

```sh
./setup.sh
. ./esp-idf/export.sh
cd network_adapter
idf.py set-target ESP_TARGET
idf.py menuconfig
idf.py build
```

Use `idf.py --preview set-target esp32s31` for ESP32-S31 with the pinned ESP-IDF revision.

Flash and monitor with the serial port connected to the ESP board:

```sh
idf.py -p SERIAL_PORT flash monitor
```

## Windows

From PowerShell:

```powershell
.\setup.ps1
.\esp-idf\export.ps1
cd network_adapter
idf.py set-target ESP_TARGET
idf.py menuconfig
idf.py build
idf.py -p SERIAL_PORT flash monitor
```

`setup.ps1` and `setup.sh` prepare the ESP-IDF revision used by this repository. See the canonical build guide before using force/update options on an existing checkout.
