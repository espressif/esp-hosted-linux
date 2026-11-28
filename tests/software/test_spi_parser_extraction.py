"""Actual production SPI classification function extraction contracts."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / ".gitlab/ci/software"))
from run_usb_parser import extract_function, extract_typed_function


class SpiParserExtractionTests(unittest.TestCase):
    def test_real_spi_parser_and_stateful_paths_extracted(self):
        source = (ROOT / "host/spi/esp_spi.c").read_text()
        functions = {x: extract_function(source, x) for x in
                     ("esp_spi_bootstrap_frame_valid",
                      "spi_is_dummy_rx",
                      "spi_is_unadvertised_idle_rx")}
        common = (ROOT / "host/include/esp_spi_common.h").read_text()
        functions["esp_spi_get_cmd_info"] = extract_typed_function(
            common, "esp_spi_get_cmd_info", "inline bool")
        functions["esp_spi_is_stateful_packet"] = extract_typed_function(
            common, "esp_spi_is_stateful_packet", "inline bool")
        rx_buf = extract_typed_function(source, "process_rx_buf", "int")
        self.assertIn("PACKET_TYPE_EVENT", functions["esp_spi_bootstrap_frame_valid"])
        self.assertIn("PACKET_TYPE_DATA", functions["esp_spi_bootstrap_frame_valid"])
        self.assertIn("hdr->if_num != 0", functions["esp_spi_bootstrap_frame_valid"])
        self.assertIn("evt->header.len", functions["esp_spi_bootstrap_frame_valid"])
        self.assertIn("esp_spi_bootstrap_frame_valid(", rx_buf)
        self.assertIn("header->if_num != 0", rx_buf)
        self.assertIn("PACKET_TYPE_COMMAND_REQUEST", functions["esp_spi_get_cmd_info"])
        self.assertIn("ESP_HCI_IF", functions["esp_spi_is_stateful_packet"])
        self.assertIn("ESP_RCP_IF", functions["esp_spi_is_stateful_packet"])
        self.assertIn("header->if_type == 0xF", functions["spi_is_dummy_rx"])
        self.assertIn("spi_is_dummy_rx(", source)
        self.assertIn("esp_spi_get_cmd_info(", source)

    def test_firmware_spi_two_stage_data_ready_contract(self):
        fw_source = (
            ROOT / "esp/esp_driver/network_adapter/main/spi_slave_api.c"
        ).read_text()
        validate_gpio = extract_typed_function(
            fw_source, "spi_validate_output_gpio", "esp_err_t"
        )
        tx_pending = extract_function(fw_source, "spi_tx_queues_pending")
        sync_dummy = extract_function(fw_source, "spi_sync_dummy_slot_data_ready")
        post_setup = extract_typed_function(
            fw_source, "spi_post_setup_cb", "void IRAM_ATTR"
        )
        queue_next = extract_typed_function(
            fw_source, "queue_next_transaction", "void"
        )
        spi_write = extract_typed_function(
            fw_source, "esp_spi_write", "int32_t"
        )
        spi_init = extract_typed_function(
            fw_source, "esp_spi_init", "interface_handle_t *"
        )
        boot_fn = fw_source.split(
            "esp_err_t send_bootup_event_to_host(", 1
        )[1].split("\n}\n", 1)[0]
        # Valid output GPIO checks precede raw register writes and bitmasks
        self.assertIn("GPIO_IS_VALID_OUTPUT_GPIO(gpio)", validate_gpio)
        for bus_pin in ("GPIO_MOSI", "GPIO_MISO", "GPIO_SCLK", "GPIO_CS"):
            self.assertIn(bus_pin, validate_gpio)
        self.assertLess(
            spi_init.index("spi_validate_output_gpio(CONFIG_ESP_SPI_GPIO_HANDSHAKE"),
            spi_init.index("spi_gpio_set_low(gpio_handshake);"),
        )
        self.assertLess(
            spi_init.index("spi_validate_output_gpio(CONFIG_ESP_SPI_GPIO_DATA_READY"),
            spi_init.index("1ULL << gpio_handshake"),
        )
        # Firmware SPI mode must come from Kconfig, not hardcoded SPI_MODE_2
        self.assertIn(".mode = CONFIG_ESP_SPI_MODE", spi_init)
        # Boot event marks PACKET_TYPE_EVENT and defers data_ready to post_setup_cb
        self.assertIn("header->packet_type = PACKET_TYPE_EVENT;", boot_fn)
        self.assertNotIn("gpio_data_ready", boot_fn)
        # Current-slot TX readiness is asserted inside spi_post_setup_cb() with handshake
        self.assertIn("slot->has_tx", post_setup)
        self.assertIn("gpio_data_ready", post_setup)
        self.assertIn("gpio_handshake", post_setup)
        # Dummy-slot DATA_READY clear must happen before rechecking spi_tx_queues_pending()
        self.assertIn("PRIO_Q_HIGH", tx_pending)
        self.assertLess(
            sync_dummy.index("spi_gpio_set_low(gpio_data_ready);"),
            sync_dummy.index("if (spi_tx_queues_pending())"),
        )
        self.assertIn("spi_gpio_set_high(gpio_data_ready);", sync_dummy)
        # queue_next_transaction() records slot->has_tx and uses spi_sync_dummy_slot_data_ready()
        self.assertIn("slot->has_tx = has_tx;", queue_next)
        self.assertIn("spi_sync_dummy_slot_data_ready()", queue_next)
        # esp_spi_write() raises data_ready after enqueue so host clocks any armed dummy slot
        self.assertIn("spi_gpio_set_high(gpio_data_ready);", spi_write)

    def test_firmware_spi_mode_kconfig_contract(self):
        kconfig = (
            ROOT / "esp/esp_driver/network_adapter/main/Kconfig.projbuild"
        ).read_text()
        spi_menu = kconfig.split('menu "SPI Configuration"', 1)[1].split("endmenu", 1)[0]
        self.assertIn("config ESP_SPI_MODE", spi_menu)
        mode_block = spi_menu.split("config ESP_SPI_MODE", 1)[1].split("config ", 1)[0]
        self.assertIn("range 0 3", mode_block)
        self.assertIn("default 2", mode_block)
        self.assertIn("spi-cpol", mode_block)
        self.assertIn("spi-cpha", mode_block)
        for mapping in (
            "Mode 0: CPOL=0, CPHA=0",
            "Mode 1: CPOL=0, CPHA=1",
            "Mode 2: CPOL=1, CPHA=0",
            "Mode 3: CPOL=1, CPHA=1",
        ):
            self.assertIn(mapping, mode_block)

        fw_source = (
            ROOT / "esp/esp_driver/network_adapter/main/spi_slave_api.c"
        ).read_text()
        self.assertNotIn("g_spi_mode", fw_source)
        self.assertNotIn(".mode = SPI_MODE_2", fw_source)
        self.assertIn("CONFIG_ESP_SPI_MODE < SPI_MODE_0 || CONFIG_ESP_SPI_MODE > SPI_MODE_3", fw_source)

    def test_missing_source_path_fails_extraction(self):
        with self.assertRaises(ValueError):
            extract_function("static bool spi_foo(void) { return true; }",
                             "spi_is_dummy_rx")


if __name__ == "__main__":
    unittest.main()
