idf_component_register(
    SRCS "nanocore_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES nanocore_midi controller_core freertos esp_timer
)
