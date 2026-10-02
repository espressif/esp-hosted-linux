# Architecture overview

Linux keeps standard networking and Bluetooth interfaces. ESP firmware owns the radio/controller side. ESP-Hosted-Linux moves control messages and data between them over SDIO, SPI, or USB.

![ESP-Hosted-Linux system architecture](../assets/system-architecture.svg)

## Linux host

ESP-Hosted host code runs as Linux kernel modules and connects to standard kernel subsystems.

Wi-Fi control follows the normal `nl80211`/`cfg80211` path:

```text
wpa_supplicant / hostapd / iw
              ↓
      nl80211 / cfg80211
              ↓
      ESP-Hosted driver
```

Wi-Fi data follows the normal network-device path:

```text
applications
     ↓
Linux network stack
     ↓
   wlanX
     ↓
ESP-Hosted driver
```

Bluetooth over the hosted SDIO/SPI/USB link uses Linux HCI:

```text
BlueZ / HCI user space
        ↓
    Linux HCI
        ↓
ESP-Hosted driver
```

Normal Wi-Fi and Bluetooth applications do not need a private ESP-Hosted user-space API. Module parameters and debugfs entries exist for setup, diagnostics, raw transport testing, OTA, and similar maintenance functions.

When Bluetooth HCI is routed over UART instead, Linux attaches the UART HCI device through its normal Bluetooth UART path rather than sending HCI through the ESP-Hosted SDIO/SPI/USB driver.

## ESP firmware

ESP firmware lives in `esp/esp_driver/network_adapter/` and uses ESP-IDF radio/controller and peripheral components.

It handles:

- SDIO, SPI, or USB peripheral transport
- host command processing and firmware responses/events
- Wi-Fi data forwarding
- Bluetooth HCI forwarding when HCI shares the hosted transport

## Transport payload

SDIO, SPI, and USB use the same packed 12-byte ESP-Hosted header before payload data. `offset` may be larger than 12 when DMA alignment padding is inserted.

| Field | Size | Purpose |
|---|---:|---|
| Interface type | 4 bits | STA, AP, HCI, internal, or test interface |
| Interface number | 4 bits | Interface instance |
| Flags | 1 byte | Packet flags such as `MORE_FRAGMENT` |
| Packet type | 1 byte | Data, command request/response, event, or EAPOL |
| Reserved 1 | 1 byte | Reserved |
| Packet length | 2 bytes | Payload length |
| Payload offset | 2 bytes | Byte offset from buffer start to payload |
| Checksum | 2 bytes | Software checksum when checksum capability is enabled |
| Packet flag / `reserved2` | 1 byte | Internal packet metadata; also used for wake-packet marking |
| Interface-specific byte | 1 byte | HCI packet type or private/test metadata; reserved when unused |

Multi-byte header fields are carried in little-endian wire order by current host and firmware code.

[SDIO](sdio.md) and [SPI](spi.md) describe bus-specific framing and transfer behavior.
