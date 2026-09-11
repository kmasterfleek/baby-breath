set(SRCS
    "main.c" "csi_collector.c" "stream_sender.c" "nvs_config.c"
    "edge_processing.c" "ota_update.c" "power_mgmt.c"
    "wasm_runtime.c" "wasm_upload.c" "rvf_parser.c"
    "mmwave_sensor.c"
    "swarm_bridge.c"
    "ble_prov.c"
)

set(REQUIRES "")

# ADR-061: Mock CSI generator for QEMU testing
if(CONFIG_CSI_MOCK_ENABLED)
    list(APPEND SRCS "mock_csi.c")
endif()

# ADR-045: AMOLED display support (compile-time optional)
if(CONFIG_DISPLAY_ENABLE)
    if(CONFIG_DISPLAY_BOARD_HOSYOND_28_ILI9341)
        # Hosyond 2.8" ILI9341: UDP-fed heatmap strips, no LVGL, no touch.
        list(APPEND SRCS "display_hal_ili9341.c" "display_heat.c")
        set(REQUIRES esp_lcd esp_lcd_ili9341)
    else()
        # Waveshare 1.69" ST7789V2 + CST816: LVGL UI (unchanged default path).
        list(APPEND SRCS "display_hal.c" "display_ui.c" "display_task.c")
        set(REQUIRES esp_lcd esp_lcd_touch lvgl)
    endif()
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${REQUIRES}
)
