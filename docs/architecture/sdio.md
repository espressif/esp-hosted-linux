# SDIO transport

ESP-Hosted-Linux uses an ESP SDIO-slave peripheral and a Linux SDIO function driver to exchange hosted packets. The SDIO transport is available on ESP32, ESP32-C5, ESP32-C6, and ESP32-C61 in this repository.

## SDIO function

Linux binds to the ESP SDIO function and uses standard SDIO block/byte APIs. Host code uses a 512-byte block size and a 15,872-byte receive/aggregation buffer.

`host/sdio/esp_sdio_decl.h` defines these IDs:

| Vendor ID | Device IDs | Targets |
|---|---|---|
| `0x6666` | `0x2222`, `0x3333` | ESP32 |
| `0x0092` | `0x6666`, `0x7777` | ESP32-C5, ESP32-C6, ESP32-C61 |

Device IDs describe what the driver can bind to. [Supported hardware](../reference/supported-hardware.md) is the user-facing target reference.

## Register interface

Slave-host registers start at `0x3FF55000`. Host code uses these key addresses:

| Register | Address | Purpose |
|---|---:|---|
| Token/buffer counter | `0x3FF55044` | ESP receive-buffer availability |
| Interrupt status | `0x3FF55058` | ESP-to-host interrupt/status bits |
| Packet-length counter | `0x3FF55060` | Accumulated ESP-to-host data length |
| Scratch/capability register 0 | `0x3FF5506C` | Capability and boot information |
| Scratch register 7 | `0x3FF5508C` | Host-to-ESP control/interrupt bits |

## Initialization

1. Linux enumerates the ESP SDIO function and probes the ESP-Hosted driver.
2. Host code initializes its TX-buffer and RX-byte counters from ESP counters.
3. Host and firmware exchange boot and capability information.
4. The data path opens after transport and adapter setup complete.

Transport counters are cumulative and wrap according to protocol masks. They are not one-packet flags.

## Host to ESP

Before a CMD53 write, host code checks ESP receive-buffer credits and sends only when enough buffer space is available.

Linux keeps its own transmitted-buffer count and derives credit changes from the cumulative ESP counter. Traffic can be aggregated within transport limits.

## ESP to host

When ESP has data:

1. firmware advances its accumulated packet-length counter
2. firmware raises an SDIO interrupt
3. Linux reads interrupt state and accumulated length
4. Linux calculates how many bytes are unread
5. Linux issues CMD53 read(s), validates hosted frames, and advances its local counter

The interrupt path reads a combined register window and reuses the prefetched packet-length value for the first receive operation.

## Host sleep

The host-sleep/wakeup implementation uses the SDIO power-save path plus a separate ESP-to-host wake GPIO. [Host sleep and ESP wakeup](../guides/host-sleep.md) covers that setup.

## Hosted payload

Wi-Fi data, HCI data, commands, responses, and events can share the SDIO link. The common header identifies interface and packet type. See [Architecture overview](overview.md#transport-payload).

## Bring-up checklist

If SDIO is not working, check in this order:

1. power, reset, and common ground
2. CMD/DAT pull-ups for the ESP target
3. wiring length and signal quality
4. Linux SDIO enumeration and device ID
5. interrupt delivery
6. packet-length and buffer counters
7. higher-level commands and data

[SDIO troubleshooting](../troubleshooting.md#sdio-device-is-not-detected) has command-level checks.
