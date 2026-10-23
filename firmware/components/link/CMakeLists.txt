# link_frame.c and link_ring.c are pure C (host tested in test/host/link).
idf_component_register(
    SRCS "link.c" "link_control.c" "link_gatt.c" "link_sink.c" "link_frame.c" "link_ring.c"
    INCLUDE_DIRS "include"
    REQUIRES ns_proto esp_common
    PRIV_REQUIRES board bt esp_app_format esp_timer gatt_server
)
