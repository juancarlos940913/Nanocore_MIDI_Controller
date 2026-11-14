idf_component_register(
    SRCS "display_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_lcd
        esp_driver_spi
        esp_driver_gpio
        freertos
)
