# IEEE 802.15.4 RCP (Thread and Zigbee)

ESP-Hosted-Linux exposes the ESP IEEE 802.15.4 radio as a Spinel/HDLC Radio Co-Processor (RCP) through `/dev/esp_rcp0`.

The firmware backend uses ESP-IDF's OpenThread RCP implementation, but the ESP-Hosted transport and Linux character device are deliberately stack-agnostic. Espressif's Zigbee gateway architecture also uses the OpenThread `ot_rcp` firmware through its Radio Spinel interface, so compatible Zigbee host stacks can use the same RCP protocol path.

This repository currently qualifies OpenThread end-to-end over the hosted transports. It does not ship or HIL-qualify a Linux Zigbee userspace stack, so Zigbee support here means RCP/Radio-Spinel compatibility rather than a bundled Zigbee application.

## Supported paths

| ESP target | Hosted transport | Firmware default |
|---|---|---|
| ESP32-C6 | SPI | RCP must be enabled in firmware |
| ESP32-C6 | SDIO | RCP must be enabled in firmware |
| ESP32-S31 | USB | RCP support is included in the S31 defaults |
| ESP32-S31 | SPI | RCP support is available with SPI selected |

Project qualification includes real Spinel exchange and real Thread RF traffic on C6/SPI, C6/SDIO, and S31/USB.

## Data path

```text
Thread/OpenThread POSIX or a
compatible Zigbee Radio Spinel host
                 |
          Spinel over HDLC
                 |
          /dev/esp_rcp0
                 |
        ESP-Hosted driver
                 |
          SDIO / SPI / USB
                 |
        ESP-Hosted firmware
                 |
       ESP-IDF OpenThread RCP
                 |
         IEEE 802.15.4 radio
```

Wi-Fi remains the primary ESP-Hosted service. Bluetooth HCI and the hosted IEEE 802.15.4 RCP are secondary services selected by the host.

## Enable the RCP service

The module parameter `radio_service` accepts:

| Value | Hosted secondary services |
|---|---|
| `none` | none |
| `bt` | Bluetooth HCI |
| `154` | IEEE 802.15.4 RCP |
| `bt+154` | Bluetooth HCI and IEEE 802.15.4 RCP |

Enable the RCP while loading a module, for example:

```sh
sudo insmod ./esp32_usb.ko radio_service=154
```

To run hosted Bluetooth and the RCP together:

```sh
sudo insmod ./esp32_usb.ko radio_service=bt+154
```

The service can also be changed after module load:

```sh
echo 154 | sudo tee /sys/module/esp32_usb/parameters/radio_service
cat /sys/module/esp32_usb/parameters/radio_service_active
```

Use `esp32_sdio` or `esp32_spi` in the sysfs path for those transports.

Adding a service is applied in place. Removing an active Bluetooth or RCP service requires a controlled firmware restart; the driver reapplies the requested service set after recovery.

## Check the RCP device

After firmware confirms `154` as active:

```sh
ls -l /dev/esp_rcp0
dmesg | grep -i rcp
```

A normal registration message includes the RCP generation:

```text
RCP byte-stream endpoint registered as /dev/esp_rcp0 generation=1
```

Only one userspace process may own the RCP stream at a time. A second opener receives `EBUSY`.

For non-root use, set device permissions with a udev rule appropriate for the distribution, for example:

```text
KERNEL=="esp_rcp0", GROUP="dialout", MODE="0660"
```

## Run OpenThread POSIX

For Thread, start `ot-daemon` with the ESP-Hosted character device:

```sh
sudo ot-daemon -v 'spinel+hdlc+uart:///dev/esp_rcp0'
```

Then use `ot-ctl`:

```sh
sudo ot-ctl state
sudo ot-ctl version
```

To form a Thread network:

```sh
sudo ot-ctl dataset init new
sudo ot-ctl dataset commit active
sudo ot-ctl ifconfig up
sudo ot-ctl thread start
sudo ot-ctl state
sudo ot-ctl ipaddr
```

## Use the RCP with Zigbee

The RCP byte stream is not Thread-specific at the ESP-Hosted layer. A Zigbee host implementation that supports Espressif/OpenThread Radio Spinel can open the same `/dev/esp_rcp0` endpoint and use the ESP IEEE 802.15.4 radio.

Espressif's Zigbee gateway uses an `ot_rcp` image as the remote 802.15.4 radio and communicates with it through Radio Spinel. On ESP-Hosted-Linux, SDIO/SPI/USB carries the same Spinel byte stream instead of a dedicated RCP UART.

ESP-Hosted-Linux does not currently provide a Zigbee daemon or Zigbee-specific userspace wrapper. End-to-end Zigbee qualification should therefore use the intended Linux Zigbee host stack and be tracked separately from the existing OpenThread HIL qualification.

## Firmware configuration

### ESP32-S31

`sdkconfig.defaults.esp32s31` includes the hosted Spinel RCP configuration using the ESP-IDF OpenThread RCP backend:

```text
CONFIG_ESP_HOSTED_RCP=y
CONFIG_ESP_HOSTED_RCP_OPENTHREAD=y
CONFIG_OPENTHREAD_ENABLED=y
CONFIG_OPENTHREAD_RADIO=y
CONFIG_OPENTHREAD_RADIO_NATIVE=y
CONFIG_OPENTHREAD_RCP_CUSTOM=y
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
```

The RCP backend is compiled into the product firmware but remains inactive until the host requests `radio_service=154` or `bt+154`.

### ESP32-C6

ESP32-C6 has an IEEE 802.15.4 radio, but `sdkconfig.defaults.esp32c6` does not enable the hosted RCP by default. Enable `CONFIG_ESP_HOSTED_RCP=y` together with the OpenThread radio/custom-RCP options shown above when building a C6 RCP image. The repository setup step applies the custom-RCP compatibility patch required by the pinned ESP-IDF revision.

C6 OpenThread RCP has been physically qualified over SPI and SDIO.

## Dedicated RCP UART on ESP32-S31

ESP32-S31 can route the OpenThread/Spinel RCP to a UART instead of the ESP-Hosted transport. This is an explicit alternative for products that require physically separate host interfaces.

Disable hosted RCP and select the UART backend:

```text
CONFIG_ESP_HOSTED_RCP=n
CONFIG_OPENTHREAD_RCP_CUSTOM=n
CONFIG_OPENTHREAD_RCP_UART=y
CONFIG_ESP_THREAD_RCP_UART=y
```

The project defaults are UART2 at 460800 baud with hardware flow control disabled. Set `CONFIG_ESP_THREAD_RCP_UART_TX_PIN` and `CONFIG_ESP_THREAD_RCP_UART_RX_PIN` for the board. Bluetooth HCI can use a different UART, such as UART1.

When the RCP uses the dedicated UART, Linux talks to that serial device directly; `/dev/esp_rcp0` is used only by the hosted RCP path.

## Session and recovery behavior

Spinel is a stateful stream, so ESP-Hosted does not continue silently after ambiguous transport loss.

- Each userspace open establishes a generation/nonce boundary with firmware before RCP data is accepted.
- Closing a live `/dev/esp_rcp0` session triggers a controlled firmware reincarnation. This prevents a later opener from inheriting asynchronous RCP work from the closed session.
- A transport or framing failure that could truncate an active Spinel stream is fail-closed and triggers recovery.
- File descriptors from an old firmware generation become invalid; userspace can see `ENODEV`.
- A firmware restart can also interrupt Wi-Fi or hosted Bluetooth state.

A service manager should restart the host RCP process when it exits after firmware recovery.

## Troubleshooting

Check requested and active services first:

```sh
cat /sys/module/esp32_usb/parameters/radio_service
cat /sys/module/esp32_usb/parameters/radio_service_active
dmesg | grep -i -E 'rcp|radio service'
```

Then check the device and host stack:

```sh
ls -l /dev/esp_rcp0
```

For OpenThread, also check:

```sh
sudo ot-ctl state
```

Use the module name for the active transport. See [Troubleshooting](../troubleshooting.md#ieee-802154-rcp-device-is-missing-or-the-host-stack-disconnects).
