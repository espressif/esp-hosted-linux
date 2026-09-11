# SPI transport

ESP-Hosted-Linux uses full-duplex SPI plus two required ESP-to-host GPIOs to coordinate transfers between Linux and ESP firmware.

## Signals

Along with MOSI, MISO, SCLK, chip select, reset, and ground, ESP-Hosted SPI requires:

**Handshake** — ESP is ready for a transaction.

**Data Ready** — ESP has a packet waiting for Linux.

Both signals must be assigned in the ESP firmware and Linux host configuration and physically connected between the ESP and host. They are required for normal SPI operation.

Linux can start a transfer when it has data to send, but it waits for ESP readiness before clocking the bus.

A Linux platform must map the SPI controller, chip select, reset GPIO, Handshake, and Data Ready through its normal device-tree/pinctrl or platform integration.

## Transfer size, mode, and clock

Host-side framing uses `SPI_BUF_SIZE = 1600` bytes and SPI mode 2. Hosted frames include the 12-byte common header plus any alignment padding.

Linux starts SPI at 10 MHz so the boot exchange can begin. Firmware includes its target SPI clock in boot information, and the host can reconfigure the SPI device after receiving that value. The final clock therefore depends on the ESP target and host-controller limits.

## Transfer sequence

1. ESP prepares RX storage for the next full-duplex transfer.
2. If ESP has data, it places a packet in TX and asserts Data Ready.
3. ESP asserts Handshake when a transfer can start.
4. Linux clocks a full-duplex transaction when either side has work.
5. Both sides inspect the hosted header. A zero payload length means no packet in that direction.
6. ESP prepares the next transaction.

Linux keeps internal/control traffic, HCI traffic, and normal data in separate priority queues before sending them over SPI.

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

SPI uses the common header described in [Architecture overview](overview.md#transport-payload).

[Hardware setup](../getting-started/hardware-setup.md#spi) has the required signals and ESP pin assignments. [SPI troubleshooting](../troubleshooting.md#spi-does-not-receive-the-boot-event) covers bring-up failures.
