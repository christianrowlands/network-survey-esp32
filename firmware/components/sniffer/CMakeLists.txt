# raw_frame.c is pure C (host tested in test/host/raw).
idf_component_register(
    SRCS "sniffer.c" "sniffer_radio.c" "sniffer_raw.c" "sniffer_debug.c" "raw_frame.c"
    INCLUDE_DIRS "include"
    REQUIRES aggregator ns_proto esp_common
    PRIV_REQUIRES channel_planner dot11_parser esp_wifi esp_event esp_timer esp_ringbuf log
)
