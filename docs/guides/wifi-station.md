# Wi-Fi station

ESP-Hosted-Linux exposes a normal Linux WLAN interface. Find its name first:

```sh
iw dev
```

Examples below use `wlan0`. Replace it if ESP-Hosted registered another `wlanX` interface.

## Bring the interface up

```sh
sudo ip link set wlan0 up
```

## Scan

```sh
sudo iw dev wlan0 scan
```

## Connect with `wpa_supplicant`

Your distribution may already manage the interface through NetworkManager, systemd-networkd, or another service. Keep using that service for normal system integration. Direct `wpa_supplicant` commands are useful for bring-up and debugging.

### Open network

Create `open.conf`:

```text
network={
    ssid="MY_OPEN_SSID"
    key_mgmt=NONE
}
```

Start `wpa_supplicant`:

```sh
sudo wpa_supplicant -D nl80211 -i wlan0 -c ./open.conf
```

### WPA2-Personal

Generate a configuration:

```sh
wpa_passphrase "MY_SSID" "MY_PASSPHRASE" > wpa2.conf
sudo wpa_supplicant -D nl80211 -i wlan0 -c ./wpa2.conf
```

### WPA3-Personal (SAE)

Example `wpa3.conf`:

```text
network={
    ssid="MY_WPA3_SSID"
    sae_password="MY_WPA3_PASSPHRASE"
    key_mgmt=SAE
    ieee80211w=2
}
```

Start it with:

```sh
sudo wpa_supplicant -D nl80211 -i wlan0 -c ./wpa3.conf
```

Security support depends on ESP target, firmware, kernel, `wpa_supplicant`, and AP configuration.

## Check association

```sh
iw dev wlan0 link
```

## Get an IP address

IP configuration stays on Linux. Use your normal network manager or DHCP client.

If `dhclient` is installed:

```sh
sudo dhclient -v wlan0
```

Then check address, route, and connectivity:

```sh
ip address show dev wlan0
ip route
ping <gateway-or-peer-address>
```

## Disconnect

If you started a dedicated `wpa_supplicant` in the foreground as shown above, stop it with `Ctrl-C`.

For a background `wpa_supplicant` that exposes a control socket:

```sh
sudo wpa_cli -i wlan0 disconnect
```

If no supplicant owns the interface, `iw` can request a direct disconnect:

```sh
sudo iw dev wlan0 disconnect
```

## If connection fails

Collect:

- verbose `wpa_supplicant` output
- host `dmesg`
- ESP serial log
- an over-the-air capture for authentication or association failures

[Wi-Fi troubleshooting](../troubleshooting.md#authentication-or-association-fails) lists the next checks.
