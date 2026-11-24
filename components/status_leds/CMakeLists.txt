idf_component_register(
    SRCS "status_leds.c"
    INCLUDE_DIRS "include"
    REQUIRES board controller_core freertos esp_timer
)
