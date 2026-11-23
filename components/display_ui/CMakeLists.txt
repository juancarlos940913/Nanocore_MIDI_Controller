idf_component_register(
    SRCS "display_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board
        controller_core
        nanocore_map
        name_editor
        esp_lcd
        esp_lcd_st7796
        esp_driver_spi
        freertos
)
