# Troubleshooting

Start at the transport and work upward:

**power/wiring → bus enumeration → ESP boot exchange → WLAN/HCI registration → Wi-Fi/Bluetooth behavior**

If the bus is not working, debugging `wpa_supplicant` or BlueZ first will not help.

## Collect these first

```sh
uname -a
iw dev
ip link
lsmod | grep -E 'esp32|cfg80211|bluetooth'
dmesg
```

Also save the ESP serial log. Record ESP target, transport, firmware revision, Linux kernel, host board, and steps to reproduce.

For Wi-Fi protocol failures, capture traffic over the air when possible. For Bluetooth, use `btmon`.

## SDIO device is not detected

1. Check power, reset, CMD/DAT wiring, grounds, and target-specific pull-ups.
2. Shorten wiring and lower clock speed for bring-up.
3. Confirm the MMC/SDIO controller and pinctrl are enabled in Linux.
4. Check enumeration:

   ```sh
   ls /sys/bus/sdio/devices
   dmesg | grep -i -E 'mmc|sdio|esp'
   ```

5. ESP-Hosted host code recognizes IDs `6666:2222`, `6666:3333`, `0092:6666`, and `0092:7777`.
6. If enumeration works but transfers fail, look for CMD53 or interrupt errors, then inspect the length/buffer counters in [SDIO transport](architecture/sdio.md).

Do not copy ESP32-specific electrical or timing workarounds to newer targets without checking that target first.

## SPI does not receive the boot event

Check:

- Device Tree node `compatible = "espressif,esp32-spi"` is applied and driver is bound
- host reset GPIO defined in Device Tree really resets ESP
- ESP firmware is built for SPI
- `spidev` does not own the same bus/CS
- host and ESP use matching SPI mode (mode 2)
- Handshake and Data Ready GPIOs are described with correct active polarity in Device Tree and generate interrupts
- wiring is short with a good ground
- SPI clock is low enough for initial bring-up

The host starts SPI in mode 2 at 10 MHz. After boot information arrives it can switch to the clock advertised by firmware.

If the controller keeps CS asserted longer than ESP expects, check firmware option `ESP_SPI_DEASSERT_HS_ON_CS`.

## USB device is not detected

1. Check physical USB cable connection and host USB port power.
2. Confirm ESP32-S31 firmware is configured and built for USB transport (`CONFIG_ESP_USB_HOST_INTERFACE=y`).
3. Check USB device enumeration in Linux:

   ```sh
   lsusb -d 303a:4002
   dmesg | grep -i -E 'usb|esp32_usb'
   ```

   The device should appear with vendor ID `303a` and product ID `4002` (Espressif ESP32-S31 Hosted Network Adapter).
4. Verify that the `esp32_usb` kernel module is loaded:

   ```sh
   lsmod | grep esp32_usb
   ```

5. If `esp32_usb` is loaded and the USB device enumerates, but WLAN does not appear, check `dmesg` for endpoint allocation errors or firmware boot notification timeouts.

## WLAN interface is missing

First confirm the ESP boot exchange succeeded in host and ESP logs.

Then run:

```sh
lsmod | grep esp32
iw dev
ip link
```

If the ESP WLAN interface exists but is down, bring up the interface name reported by `iw dev`:

```sh
sudo ip link set <wlan-interface> up
```

If no ESP WLAN interface appears, save the full module-load `dmesg` and ESP boot log. The final user-space error alone is usually not enough.

## Wi-Fi connects but data does not pass

Use the ESP interface name shown by `iw dev`:

```sh
ip address show dev <wlan-interface>
ip route
iw dev <wlan-interface> link
```

Then narrow the problem to Linux IP setup, hosted transport, ESP radio path, or over-the-air traffic. Capture counters and a wireless trace when useful.

## Authentication or association fails

Collect verbose `wpa_supplicant` or `hostapd` logs, host `dmesg`, ESP logs, and an air capture.

Check that AP security, PMF settings, band/channel, and firmware capability match the ESP target you are running.

## Bluetooth controller is missing

For HCI over SDIO/SPI/USB:

```sh
bluetoothctl list
sudo btmon
```

Confirm Bluetooth is enabled in ESP firmware and Linux has the `bluetooth` module.

For UART HCI also check:

- TX/RX direction and pinmux
- CTS/RTS if hardware flow control is enabled
- matching baud rate
- matching flow-control mode (`flow` vs `noflow`)
- no serial console or onboard Bluetooth service owns the UART
- Linux HCI-UART attachment completed successfully

## Host and firmware do not complete the boot/command exchange

When debugging protocol or command failures, start with host driver and ESP firmware from the same repository revision or release. Mixing versions can introduce capability or command-format differences.

Save both sides of the boot exchange before changing higher-level Wi-Fi configuration.

## Unknown SDIO kernel symbols

If module loading reports unresolved MMC/SDIO symbols, check that the running kernel provides the required support as built-in code or modules.

On some systems, loading the relevant SDHCI/MMC module resolves this. If not, make sure ESP-Hosted was built against the exact running kernel build tree and matching `Module.symvers`.

## OTA fails

Keep host `dmesg` and ESP serial logs.

Find where the update stopped: file access, transfer, flash write, ESP restart, or link re-enumeration.

After an apparent success, check firmware version and make sure the hosted link returns. Transfer completion alone does not prove the new image booted.

## Reporting an issue

Include:

- ESP target and board/module
- transport and bus clock or UART baud rate
- Linux host and kernel
- firmware/host commit or release
- host `dmesg`
- ESP serial log
- relevant `wpa_supplicant`, `hostapd`, BlueZ, or `btmon` logs
- air capture for Wi-Fi protocol failures when available
