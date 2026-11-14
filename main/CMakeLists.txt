idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_event
        bt
        esp_driver_gpio
        nanocore_midi
        display_ui
)
