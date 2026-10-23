idf_component_register(
    SRCS "app_main.c" "pairing_cmds.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board ble_sniffer gatt_server gps link ota sniffer ui_io nvs_flash esp_app_format
                  aggregator channel_planner dot11_parser
)
