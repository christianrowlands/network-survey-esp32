idf_component_register(
    SRCS "board.c"
    INCLUDE_DIRS "include"
    REQUIRES ns_proto
    PRIV_REQUIRES esp_adc esp_driver_gpio esp_hw_support
)
