# Performance and raw transport

Throughput depends on ESP target, radio mode, Linux host, bus clock, kernel, and test setup. Values below are reference measurements carried forward from project testing, not guaranteed performance.

## Wi-Fi throughput

Best recorded Wi-Fi throughput from available project measurements:

| ESP target | Transport | Peak TCP | Peak UDP |
|---|---|---:|---:|
| ESP32 | SDIO | 43.5 Mbps | 49.1 Mbps |
| ESP32-C3 | SPI | 15.8 Mbps | 17.1 Mbps |
| ESP32-C5 | SDIO | 63.3 Mbps | 97.8 Mbps |
| ESP32-C6 | SDIO | 55.6 Mbps | 90.4 Mbps |
| ESP32-C61 | SDIO | 42.6 Mbps | 64.0 Mbps |

Peak values are maxima from available measurements and may come from different Tx/Rx directions or radio configurations. Targets without comparable recorded measurements are omitted; a missing row does not mean that target is unsupported.

## Detailed measurements

<details markdown="1">
<summary><strong>Show Tx/Rx measurements and radio modes</strong></summary>

| ESP target | Transport / radio mode | TCP Tx | TCP Rx | UDP Tx | UDP Rx |
|---|---|---:|---:|---:|---:|
| ESP32 | SDIO 2.4 GHz, 40 MHz | 43.5 Mbps | 24.8 Mbps | 47.6 Mbps | 49.1 Mbps |
| ESP32 | SDIO 2.4 GHz, 20 MHz | 32.4 Mbps | 28.3 Mbps | 36.1 Mbps | 41.7 Mbps |
| ESP32 | SPI | 7.47 Mbps | 7.30 Mbps | 7.39 Mbps | 7.32 Mbps |
| ESP32-C3 | SPI | 15.8 Mbps | 15.2 Mbps | 17.1 Mbps | 14.9 Mbps |
| ESP32-C5 | SDIO 2.4 GHz, 11n 40 MHz | 62.3 Mbps | 60.3 Mbps | 97.2 Mbps | 81.7 Mbps |
| ESP32-C5 | SDIO 2.4 GHz, 11ax 20 MHz | 52.7 Mbps | 42.4 Mbps | 66.2 Mbps | 49.4 Mbps |
| ESP32-C5 | SDIO 5 GHz, 11n 40 MHz | 63.3 Mbps | 52.5 Mbps | 97.8 Mbps | 81.3 Mbps |
| ESP32-C5 | SDIO 5 GHz, 11ax 20 MHz | 54.8 Mbps | 47.6 Mbps | 68.0 Mbps | 65.0 Mbps |
| ESP32-C6 | SDIO 2.4 GHz, 40 MHz | 41.7 Mbps | 51.0 Mbps | 90.4 Mbps | 58.4 Mbps |
| ESP32-C6 | SDIO 2.4 GHz, 20 MHz | 40.9 Mbps | 55.6 Mbps | 67.8 Mbps | 68.3 Mbps |
| ESP32-C6 | SPI | 16.2 Mbps | 16.9 Mbps | 17.5 Mbps | 17.2 Mbps |
| ESP32-C61 | SDIO 2.4 GHz, 40 MHz | 42.3 Mbps | 41.5 Mbps | 57.2 Mbps | 45.3 Mbps |
| ESP32-C61 | SDIO 2.4 GHz, 20 MHz | 42.6 Mbps | 40.7 Mbps | 64.0 Mbps | 44.6 Mbps |
| ESP32-C61 | SPI | 13.3 Mbps | 13.6 Mbps | 13.9 Mbps | 14.0 Mbps |

Recorded host details are incomplete for some rows. Available notes are:

- ESP32-C5 SDIO used Raspberry Pi 5 with a 50 MHz SDIO clock.
- ESP32, ESP32-C3, ESP32-C6, and ESP32-C61 measurements used Raspberry Pi 4B; SDIO cases used 41.67 MHz.

`Tx` and `Rx` labels are retained from the original result set because complete traffic-direction methodology was not recorded alongside every row. Treat these numbers as comparison data, not as a reproducible benchmark specification.

A missing target/transport row does **not** mean that combination is unsupported. It means no comparable measurement is recorded in this table.

</details>

For new results, record at least:

- Linux host and kernel
- ESP target and firmware revision
- band, channel, PHY mode, and channel width
- SDIO/SPI clock
- test tool and version
- traffic direction and endpoint roles
- network topology and AP/peer details

## Raw transport throughput

Raw mode bypasses the normal Wi-Fi data path and stresses SDIO or SPI directly. It is useful for bus bring-up and transport testing; it is not a Wi-Fi application benchmark.

Raw throughput testing requires `TEST_RAW_TP` in `host/include/esp_stats.h`. Build the matching module, then select the direction with the `raw_tp_mode` module parameter:

| `raw_tp_mode` | Direction |
|---:|---|
| `1` | Host → ESP |
| `2` | ESP → Host |

SDIO example:

```sh
sudo insmod ./esp32_sdio.ko resetpin=GPIO_NUMBER raw_tp_mode=1
```

SPI example:

```sh
sudo insmod ./esp32_spi.ko resetpin=GPIO_NUMBER raw_tp_mode=2
```

Replace `GPIO_NUMBER` with the Linux GPIO connected to ESP reset/enable. For SPI, Handshake and Data Ready must also be configured and connected as described in [Hardware setup](../getting-started/hardware-setup.md#spi).
