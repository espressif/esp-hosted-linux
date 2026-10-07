set(COMPONENT_SRCS "app_main.c" "slave_bt.c" "cmd.c" "stats.c")

if(CONFIG_ESP_HOSTED_RCP)
    list(APPEND COMPONENT_SRCS "esp_rcp.c")
    # esp_rcp.c uses esp_random() to bind each session to one firmware boot.
    list(APPEND COMPONENT_REQUIRES esp_hw_support)
    if(CONFIG_ESP_HOSTED_RCP_OPENTHREAD)
        list(APPEND COMPONENT_SRCS "esp_rcp_openthread.c")
        # esp_rcp_openthread.c directly includes esp_coexist.h and
        # esp_vfs_eventfd.h; OpenThread keeps both as private dependencies.
        list(APPEND COMPONENT_REQUIRES openthread esp_coex vfs)
    endif()
endif()

if(CONFIG_ESP_THREAD_RCP_UART)
    list(APPEND COMPONENT_SRCS "thread_rcp_uart.c")
    list(APPEND COMPONENT_REQUIRES openthread esp_coex vfs esp_event)
endif()
set(COMPONENT_ADD_INCLUDEDIRS "./include")

if(CONFIG_ESP_SDIO_HOST_INTERFACE)
    list(APPEND COMPONENT_SRCS sdio_slave_api.c)
elseif(CONFIG_ESP_SPI_HOST_INTERFACE)
    list(APPEND COMPONENT_SRCS spi_slave_api.c)
elseif(CONFIG_ESP_USB_HOST_INTERFACE)
    list(APPEND COMPONENT_SRCS usb/esp_usb_device.c)
endif()


register_component()

target_compile_definitions(${COMPONENT_LIB} PRIVATE)

# Add directory of protocomm_priv.h to include paths
idf_component_get_property(protocomm_dir protocomm COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${protocomm_dir}/src/common")

# Redirect IDF's esp_supplicant_init/deinit (called unconditionally by
# esp_wifi's wifi_init.c) to the __wrap_ no-ops in cmd.c. This lets
# esp_wpa_main.o link for the RSN/WPA IE parser (registered as
# wpa_cb.wpa_parse_wpa_ie in cmd.c) while --gc-sections drops the rest of
# the supplicant state machine from the final image.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_supplicant_init"
    "-Wl,--wrap=esp_supplicant_deinit")
