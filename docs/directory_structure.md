### Directory structure for Linux based host

```
├── docs                            (Documentation)
├── esp                             (ESP firmware directory)
│   └── esp_driver                  (ESP IDF network adapter firmware)
├── host                            (Host driver directory for Linux based host)
│   ├── rpi_init.sh                 (Installation sequence for ESP-HOSTED-Linux driver)
│   │   └── spidev_disabler.dts     (dts file for SPI transport)
│   ├── sdio                        (Contains SDIO transport files used by kernel module)
│   └── spi                         (Contains SPI transport files used by kernel module)
```
