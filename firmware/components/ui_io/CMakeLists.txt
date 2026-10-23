idf_component_register(
    SRCS "ui_io.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio esp_timer
)
