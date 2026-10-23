# nmea.c is pure C (host tested in test/host/nmea).
idf_component_register(
    SRCS "gps.c" "nmea.c"
    INCLUDE_DIRS "include"
    REQUIRES ns_proto esp_common
    PRIV_REQUIRES board esp_driver_uart esp_driver_gpio esp_timer
)
