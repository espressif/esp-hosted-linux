# Toolchain and kernel
CROSS_COMPILE :=
KERNEL := /lib/modules/$(shell uname -r)/build
ARCH := $(shell uname -m)

# Normalize ARCH
ifeq ($(ARCH), x86_64)
    ARCH := x86
endif
ifeq ($(ARCH), aarch64)
    ARCH := arm64
endif
ifneq ($(filter armv%,$(ARCH)),)
    ARCH := arm
endif

# Module name (default: sdio)
target ?= sdio
MODULE_NAME := esp32_$(target)

# Device Tree overlay compiler and generated Raspberry Pi SPI overlays.
DTC ?= dtc
SPI_DTBOS := spidev_disabler.dtbo overlays/esp32-spi.dtbo overlays/esp32-spi-rpi3b-plus.dtbo overlays/esp32-spi-hd-rpi5.dtbo
SDIO_DTBOS := overlays/esp32-sdio-pwrseq.dtbo

# Debug and AP support
# NOTE: use ccflags-y (not EXTRA_CFLAGS) for -D defines. Modern kbuild
# (kernel 6.x, confirmed on 6.18) ignores EXTRA_CFLAGS, which silently
# dropped these defines and compiled out the debug stats.
CONFIG_DEBUG_LOGS ?= y
ifeq ($(CONFIG_AP_SUPPORT), y)
    ccflags-y += -DCONFIG_AP_MODE
endif
ifeq ($(CONFIG_DEBUG_STATS), y)
    ccflags-y += -DESP_DEBUG_STATS
endif

# Source and include paths
PWD := $(shell pwd)
ccflags-y += -I$(src) -I$(src)/include
EXTRA_CFLAGS += -I$(M) -I$(M)/include

# Interface-specific sources
ifeq ($(target), sdio)
    ccflags-y += -I$(src)/sdio
    EXTRA_CFLAGS += -I$(M)/sdio
    module_objects += sdio/esp_sdio.o sdio/esp_sdio_api.o
endif

ifeq ($(target), spi)
    ccflags-y += -I$(src)/spi
    EXTRA_CFLAGS += -I$(M)/spi
    module_objects += spi/esp_spi.o
endif

ifeq ($(target), spi_hd)
    ccflags-y += -I$(src)/spi_hd -DESP_SPI_HD_DEBUGFS
    EXTRA_CFLAGS += -I$(M)/spi_hd
    module_objects += spi_hd/esp_spi_hd.o
endif

ifeq ($(target), usb)
    ccflags-y += -I$(src)/usb
    EXTRA_CFLAGS += -I$(M)/usb
    module_objects += usb/esp_usb.o
endif

# Common source files
module_objects += esp_bt.o esp_rcp.o main.o esp_cmd.o esp_utils.o esp_cfg80211.o esp_stats.o esp_debugfs.o esp_log.o
CFLAGS_esp_log.o = -DDEBUG

# Module build rules
obj-m := $(MODULE_NAME).o
$(MODULE_NAME)-y := $(module_objects)

# Build targets
.DELETE_ON_ERROR:
.PHONY: all incremental clean check spi-dtbo sdio-dtbo

all: clean
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL) M=$(PWD) modules

incremental:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL) M=$(PWD) modules

spi-dtbo: $(SPI_DTBOS)

sdio-dtbo: $(SDIO_DTBOS)

spidev_disabler.dtbo: spidev_disabler.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

overlays/esp32-spi.dtbo: overlays/esp32-spi.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

overlays/esp32-spi-rpi3b-plus.dtbo: overlays/esp32-spi-rpi3b-plus.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

overlays/esp32-sdio-pwrseq.dtbo: overlays/esp32-sdio-pwrseq.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

overlays/esp32-spi-hd-rpi5.dtbo: overlays/esp32-spi-hd-rpi5.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

clean:
	rm -rf *.o */*.o *.ko *.mod.c *.symvers *.order .*.cmd .tmp_versions
	rm -f $(SPI_DTBOS) $(SDIO_DTBOS)
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL) M=$(PWD) clean

check:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL) M=$(PWD) $(module_objects)
