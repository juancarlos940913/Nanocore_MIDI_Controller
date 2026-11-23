idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_event
        bt
        nanocore_midi
        display_ui
        board
        input_manager
        controller_core
        controller_storage
        tempo_engine
        status_leds
        nanocore_map
        name_editor
)
