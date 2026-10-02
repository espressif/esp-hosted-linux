# Bluetooth

ESP-Hosted-Linux registers a standard Linux HCI controller when Bluetooth is enabled in ESP firmware. ESP32 supports BR/EDR and BLE; the other Bluetooth-capable targets in this repository use BLE. [Supported hardware](../reference/supported-hardware.md) has the target summary.

## HCI over SDIO, SPI, or USB

When Bluetooth shares the hosted transport, the kernel driver registers HCI directly with the Linux Bluetooth stack.

Check the controller with:

```sh
bluetoothctl list
bluetoothctl show
```

For HCI tracing:

```sh
sudo btmon
```

## HCI over UART

Some setups keep Wi-Fi on SDIO/SPI/USB and route Bluetooth HCI over UART. This needs matching configuration on both sides:

1. build ESP firmware with HCI UART enabled
2. choose the UART baud rate and flow-control mode in ESP firmware
3. wire the ESP UART as shown in [Hardware setup](../getting-started/hardware-setup.md#bluetooth-hci-over-uart)
4. configure the selected Linux UART and pinctrl
5. make sure no serial console or other service owns that UART
6. attach the serial device to the Linux Bluetooth stack

On systems that provide `hciattach`, use hardware flow control for a 4-wire setup:

```sh
sudo hciattach -s BAUD_RATE UART_DEVICE any BAUD_RATE flow
```

For a 2-wire TX/RX setup, disable flow control on both ESP and Linux:

```sh
sudo hciattach -s BAUD_RATE UART_DEVICE any BAUD_RATE noflow
```

Replace `UART_DEVICE` with the Linux serial device connected to ESP, for example `/dev/ttyS1`, and `BAUD_RATE` with the value configured in ESP firmware.

Recent BlueZ packages may provide `btattach` or a distribution-managed HCI-UART service instead. Use the attachment mechanism supported by your distribution, but keep H4 protocol, baud rate, and flow control consistent with ESP firmware.

## Scan and pair

Start `bluetoothctl`:

```sh
bluetoothctl
```

Then run:

```text
power on
agent on
default-agent
scan on
```

After finding a peer:

```text
scan off
pair <peer-address>
trust <peer-address>
connect <peer-address>
```

Use `info <peer-address>` to inspect the peer and `disconnect <peer-address>` to disconnect.

## BLE advertising and GATT

`bluetoothctl` advertising/GATT commands vary somewhat by BlueZ version. For bring-up:

1. confirm the ESP HCI controller appears in Linux
2. run `btmon` in another terminal
3. use `bluetoothctl` to power the controller and configure advertising or pairing
4. use a known BLE peer to test GATT discovery, reads, and writes

Record the BlueZ version and exact commands when results need to be reproducible.

## Diagnostics

Start with:

```sh
bluetoothctl show
sudo btmon
journalctl -u bluetooth --no-pager
```

Also save host `dmesg` and ESP serial logs. For UART HCI, check baud rate, flow control, pinmux, and whether another service or serial console still owns the UART.
