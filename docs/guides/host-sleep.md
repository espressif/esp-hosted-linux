# Host sleep and ESP wakeup

ESP-Hosted-Linux can keep the ESP side active while a Linux host suspends, then wake Linux when matching network traffic arrives. The host-wakeup path implemented here is for **SDIO**.

Reference work used an i.MX8M Mini EVK (`imx8mm-lpddr4-evk`). Another Linux SoC needs a wake-capable GPIO, device-tree integration, and a suspend mode that can wake from that GPIO.

## How it works

1. Linux configures an ESP-to-host GPIO as a wake source.
2. ESP firmware uses its configured `HOST_WAKEUP_GPIO` to drive that signal.
3. Linux enables a WoWLAN trigger and enters suspend.
4. ESP asserts the wake GPIO when matching traffic arrives.
5. Linux resumes and the SDIO data path continues.

Other system wake sources such as buttons or touch controllers are outside ESP-Hosted-Linux.

## ESP wake GPIO

Project Kconfig currently provides these SDIO defaults:

| ESP target | Default `HOST_WAKEUP_GPIO` |
|---|---:|
| ESP32 | IO22 |
| ESP32-C5 | IO5 |
| ESP32-C6 | IO4 |
| ESP32-C61 | IO5 |

The value is configurable in firmware. Check the active `HOST_WAKEUP_GPIO` setting instead of assuming the default matches your board wiring.

## Linux device tree

Configure a GPIO that can wake your host SoC. On i.MX8M Mini, the reference setup used a `gpio-keys` wake source:

```dts
gpio-keys {
    compatible = "gpio-keys";

    gpio_esp_to_host_wakeup {
        label = "gpio-esp-to-host-wakeup";
        linux,code = <KEY_POWER>;
        gpios = <&gpio3 21 GPIO_ACTIVE_HIGH>;
        wakeup-source;
        debounce-interval = <50>;
    };
};
```

GPIO controller, pinctrl, active level, and wake binding will differ on other hosts.

Check that Linux registered the interrupt:

```sh
cat /proc/interrupts | grep -i esp
```

## Prepare the host

Get a normal Wi-Fi station connection working before testing suspend.

Find the wireless PHY:

```sh
iw dev
```

Then enable a WoWLAN trigger. For example, if the PHY is `phy0`:

```sh
sudo iw phy phy0 wowlan enable magic-packet
sudo iw phy phy0 wowlan show
```

The driver advertises WoWLAN support for any-packet, magic-packet, disconnect, and 4-way-handshake triggers. Use the trigger that matches your test and kernel/user-space setup.

Reference i.MX8M Mini testing used:

```sh
echo s2idle | sudo tee /sys/power/mem_sleep
echo mem | sudo tee /sys/power/state
```

Suspend commands and supported sleep states differ between Linux platforms.

## Check suspend and wake

ESP logs should show messages similar to:

```text
FW_MAIN: Host Sleep
FW_SDIO_SLAVE: WAKE UP Host
FW_MAIN: Host Awake
```

On Linux, confirm suspend and resume in kernel logs, then verify WLAN traffic still works.

If ESP tries to wake Linux but the host stays asleep, check GPIO wiring, active level, pinctrl, `wakeup-source`, and whether that interrupt can wake the SoC from the selected sleep state.
