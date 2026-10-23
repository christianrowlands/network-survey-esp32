idf_component_register(
    SRCS "conn_track.c" "gap.c" "gatt_svc.c" "pairing.c" "pairing_policy.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board bt esp_app_format ns_proto
)
