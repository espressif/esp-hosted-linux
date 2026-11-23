# SPI transport

ESP-Hosted-Linux uses full-duplex SPI plus two required ESP-to-host GPIOs to coordinate transfers between Linux and ESP firmware.

## Signals

Along with MOSI, MISO, SCLK, chip select, reset, and ground, ESP-Hosted SPI requires:

**Handshake** — ESP is ready for a transaction.

**Data Ready** — ESP has a packet waiting for Linux.

Both signals must be assigned in the ESP firmware and Linux host configuration and physically connected between the ESP and host. They are required for normal SPI operation.

Linux can start a transfer when it has data to send, but it waits for ESP readiness before clocking the bus.

A Linux platform maps the SPI controller, chip select, reset GPIO, Handshake, and Data Ready through Device Tree.

## Device Tree integration

The SPI host driver binds to an `espressif,esp32-spi` Device Tree node rather than using hard-coded GPIO numbers or kernel module parameters (`resetpin=` has been removed from the driver). The Device Tree node specifies:

- `compatible = "espressif,esp32-spi"`: matches the Linux SPI driver
- `reg`: SPI chip-select index on the controller
- `spi-max-frequency`: controller and board safety maximum frequency (e.g. 30 MHz)
- `spi-cpol`, `spi-cpha`: standard Device Tree properties selecting the SPI clock polarity/phase mode; must match firmware `CONFIG_ESP_SPI_MODE` (default Mode 2, `spi-cpol` only):
  - Mode 0 (`CONFIG_ESP_SPI_MODE=0`): CPOL=0, CPHA=0 (neither `spi-cpol` nor `spi-cpha`)
  - Mode 1 (`CONFIG_ESP_SPI_MODE=1`): CPOL=0, CPHA=1 (`spi-cpha`)
  - Mode 2 (`CONFIG_ESP_SPI_MODE=2`, default): CPOL=1, CPHA=0 (`spi-cpol`)
  - Mode 3 (`CONFIG_ESP_SPI_MODE=3`): CPOL=1, CPHA=1 (`spi-cpol` and `spi-cpha`)
- chip select: see [Chip select polarity](#chip-select-polarity) below
- `reset-gpios`: optional GPIO descriptor for ESP reset/enable (active-low); preferred when the board exposes EN/reset
- `handshake-gpios`: required GPIO descriptor for Handshake interrupt input (active-high)
- `data-ready-gpios`: required GPIO descriptor for Data Ready interrupt input (active-high)

Example Device Tree snippet:

```dts
&spi0 {
    status = "okay";

    esp32_spi: esp32_spi@0 {
        compatible = "espressif,esp32-spi";
        reg = <0>;
        spi-max-frequency = <30000000>;
        spi-cpol;
        reset-gpios = <&gpio 6 GPIO_ACTIVE_LOW>;
        handshake-gpios = <&gpio 22 GPIO_ACTIVE_HIGH>;
        data-ready-gpios = <&gpio 27 GPIO_ACTIVE_HIGH>;
    };
};
```

### Chip select polarity

ESP-Hosted firmware uses the [ESP-IDF SPI Slave driver](https://docs.espressif.com/projects/esp-idf/en/stable/api-reference/peripherals/spi_slave.html) on every supported SoC. The same SPI Slave guide is also published per chip (for example `.../stable/esp32c6/api-reference/peripherals/spi_slave.html`); chip-specific pages add target notes, but CS timing follows the same assert/idle model. ESP-IDF documents the usual SPI convention: a transaction starts when the **host asserts** chip select and ends when CS returns to **idle** (de-assertion). The `peripherals/spi_slave/receiver` example states that the master begins a transfer **by pulling CS low** and clocking SCLK; the example also enables an internal pull-up on the CS GPIO so the line stays **high when unselected**.

Hosted `network_adapter` firmware configures `spi_slave_interface_config_t.spics_io_num` and does **not** flip CS polarity in software. The ESP slave therefore expects the Linux master to drive **active-low CS on the wire**: idle high, asserted low while clocking.

Linux Device Tree must match that electrical behavior:

- Do **not** set `spi-cs-high` on the ESP node (the Hosted driver rejects it).
- When chip select is GPIO-backed, describe the line with the platform **active-low** (inverted) GPIO flag so CS idles high and drives low during a transfer. On Raspberry Pi overlays this is `cs-gpios = <&gpio N 1>` (`GPIO_ACTIVE_LOW`).

The SPI core may set an internal `SPI_CS_HIGH` mode bit for GPIO-backed CS while gpiolib performs the electrical inversion. That is **not** the same as declaring active-high CS in Device Tree via `spi-cs-high`.

The driver queries these pins via the standard Linux GPIO descriptor API and requests edge-triggered interrupts for Handshake and Data Ready based on the descriptor polarities. When `reset-gpios` is present, recovery can perform an authoritative physical ESP reset. When it is absent, matching firmware uses the sequence-ACKed `CMD_HOST_REBOOT` control transaction; the host accepts that reboot only after the ACK has crossed the SPI wire.

## Transfer size, mode, and clock

Host-side framing uses `SPI_BUF_SIZE = 1600` bytes and defaults to SPI mode 2 (`CONFIG_ESP_SPI_MODE=2` on firmware paired with `spi-cpol` in Device Tree). Modes `0`–`3` are configurable when supported by both controllers and matched identically via Device Tree `spi-cpol`/`spi-cpha` and firmware `CONFIG_ESP_SPI_MODE`; Mode 2 is the default. Hosted frames include the 12-byte common header plus any alignment padding.

Linux clocks the boot-first/recovery exchange at a conservative 1 MHz. After a valid boot event, firmware's optional clock TLV selects the steady-state SPI rate subject to the Device Tree/protocol maximum. Compatible older firmware that omits the TLV falls back to the configured `clockspeed` value or the historical 10 MHz default.

## Transfer sequence

1. ESP prepares RX storage for the next full-duplex transfer.
2. When the armed SPI slot carries an ESP-to-host (E2H) packet (including the initial boot up event), ESP asserts both **Data Ready** and **Handshake** from `spi_post_setup_cb()` only after the slot's DMA descriptor is loaded into the SPI peripheral.
3. When ESP has no E2H packet at slot-arming time, it arms an idle dummy slot (`if_type = 0xF, if_num = 0xF`), clears **Data Ready** (rechecking the TX queues afterwards so a concurrent producer cannot lose its wakeup), and asserts **Handshake** so Linux can still initiate host-to-ESP (H2E) transfers.
4. If an E2H packet is enqueued via `esp_spi_write()` while an idle dummy slot is already armed, ESP asserts **Data Ready** ("TX pending behind armed dummy"). Linux clocks and discards the armed `0xF` dummy slot, which advances `queue_next_transaction()` to arm the queued E2H packet in the next slot.
5. Both sides inspect the received header:
   - Linux treats only the canonical `if_type = 0xF, if_num = 0xF` dummy header (or an all-zero frame when **Data Ready** was not asserted) as idle from ESP; any other zero-length RX frame is dropped as a protocol error.
   - ESP treats an all-zero H2E buffer (`len == 0`) from Linux as idle when the host only clocked the bus to service an E2H frame.
6. ESP deasserts **Handshake** for the completed slot, prepares the next transaction, and asserts **Handshake** again only after that next DMA slot is armed.
7. Linux does not treat a lingering high **Handshake** level from the just-completed slot as permission to clock again. Immediate requeue requires a fresh Handshake rising-edge generation; otherwise the Handshake interrupt for the newly armed slot schedules the next transfer. This prevents the master from clocking the bus in the interval between slave transaction completion and next-slot DMA setup.

Linux keeps control traffic, hosted Bluetooth HCI, OpenThread RCP, and normal Wi-Fi data in the ESP-Hosted priority queues before sending them over SPI.

## Sideband interrupts

Linux registers interrupts for Handshake and Data Ready. Either signal can schedule SPI work, so the driver does not need to poll the bus continuously.

If the first boot event never arrives, check GPIO assignment, direction, active level, pinmux, physical wiring, and interrupt delivery before debugging higher layers.

## Chip-select timing

Some SPI controllers keep chip select asserted after the data phase. Firmware option `ESP_SPI_DEASSERT_HS_ON_CS` under **Example Configuration → SPI Configuration** handles hosts where Handshake must follow the real CS deassertion rather than transfer completion.

Enable it only when the host controller needs that timing behavior.

## Clock and signal quality

Raise SPI speed only after repeated error-free transfers. Maximum stable speed depends on ESP target, Linux SPI controller, board routing, and wiring.

With jumper wires, shorten connections and reduce clock speed before treating corruption as a hosted-protocol problem.

## Hosted payload

Wi-Fi data, hosted Bluetooth HCI, OpenThread RCP, commands, responses, and events use the common ESP-Hosted framing on SPI. Bluetooth and RCP traffic are present only when their runtime services are active.

SPI uses the common header described in [Architecture overview](overview.md#transport-payload).

[Hardware setup](../getting-started/hardware-setup.md#spi) has the required signals and ESP pin assignments. [SPI troubleshooting](../troubleshooting.md#spi-does-not-receive-the-boot-event) covers bring-up failures.
