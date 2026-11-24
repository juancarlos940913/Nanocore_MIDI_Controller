idf_component_register(
    SRCS "nanocore_mapper.c"
    INCLUDE_DIRS "include"
    REQUIRES nanocore_protocol nanocore_spec controller_core esp_timer
)
