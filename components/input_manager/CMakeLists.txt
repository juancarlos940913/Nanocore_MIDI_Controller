idf_component_register(
    SRCS "input_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_driver_gpio freertos
)
