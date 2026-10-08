# Wi-Fi access point

AP mode uses Linux `hostapd`. Wi-Fi is independent of the `radio_service` setting used for hosted Bluetooth and Thread.

Build the host driver with AP support first.

On Raspberry Pi:

```sh
cd host
./rpi_init.sh <sdio|spi|usb> ap_support
```

Examples use `wlan0`. Replace it with the ESP-Hosted interface shown by `iw dev` if needed.

## WPA2-Personal example

A simple 2.4 GHz `hostapd.conf` is:

```text
interface=wlan0
driver=nl80211
ssid=MY_SSID
hw_mode=g
channel=6
wmm_enabled=1
ieee80211n=1
wpa=2
wpa_key_mgmt=WPA-PSK
wpa_passphrase=MY_PASSPHRASE
rsn_pairwise=CCMP
```

Run `hostapd` in the foreground during bring-up:

```sh
sudo hostapd ./hostapd.conf
```

## WPA3-Personal (SAE)

For WPA3-Personal, replace the WPA2 key-management line with:

```text
wpa_key_mgmt=SAE
ieee80211w=2
sae_pwe=2
```

Keep `wpa=2`, `wpa_passphrase`, and `rsn_pairwise=CCMP` from the base example. A recent `hostapd` build with SAE support is required.

## 5 GHz on ESP32-C5

ESP32-C5 can operate on 5 GHz. Use `hw_mode=a`, choose a channel allowed by your local regulatory rules, and configure the correct country code for the deployment.

Other targets in this repository are 2.4 GHz Wi-Fi devices; changing `hostapd.conf` cannot add a band the ESP target does not support.

## IP address and DHCP

`hostapd` controls the wireless AP. Linux still owns IP addressing and DHCP.

Example AP address:

```sh
sudo ip address add 192.168.1.1/24 dev wlan0
sudo ip link set wlan0 up
```

Minimal `dnsmasq` example:

```text
interface=wlan0
dhcp-range=192.168.1.20,192.168.1.100,255.255.255.0,24h
domain-needed
bogus-priv
```

Start or restart `dnsmasq` with your distribution's service manager.

This creates a local AP network. Internet sharing, forwarding, and NAT are host/distribution policy and are not configured by ESP-Hosted-Linux.

## Check connected clients

```sh
ip neigh show dev wlan0
ip address show dev wlan0
```

For authentication or radio failures, save `hostapd` output, host `dmesg`, ESP serial logs, and an air capture when possible.
