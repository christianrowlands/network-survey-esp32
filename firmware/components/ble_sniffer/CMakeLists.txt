# ble_ad.c and ble_agg.c are pure C (host tested in test/host/ble).
idf_component_register(
    SRCS "ble_sniffer.c" "ble_ad.c" "ble_agg.c"
    INCLUDE_DIRS "include"
    REQUIRES ns_proto esp_common
    PRIV_REQUIRES bt esp_timer esp_ringbuf log heap
)
