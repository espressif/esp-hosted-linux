# Repository layout

ESP firmware and the Linux host driver live at repository root:

```text
.
├── docs/                         Documentation source, assets, and styles
├── docs-theme/                   MkDocs template overrides
├── docs-html/                    Generated static documentation
├── esp/
│   └── esp_driver/
│       └── network_adapter/      ESP-Hosted firmware application
├── host/                         Linux host driver
│   ├── include/                  Common host headers
│   ├── sdio/                     SDIO transport implementation
│   ├── spi/                      SPI transport implementation
│   ├── rpi_init.sh               Optional Raspberry Pi reference helper
│   └── Makefile                  Out-of-tree kernel-module build
├── tools/                        Project utilities
├── mkdocs.yml                    Documentation build configuration
├── README.md                     Project overview
├── VERSION                       Project version
└── ORIGIN.md                     Repository split history
```

`docs-html/` is generated automatically from `docs/`, `docs-theme/`, and `mkdocs.yml`. Do not edit generated HTML by hand.

For work moved from the previous `esp-hosted` repository, use the [migration guide](../migration.md).
