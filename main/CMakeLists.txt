idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_event
        bt
        nanocore_midi
        nanocore_protocol
        nanocore_mapper
        nanocore_spec
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
