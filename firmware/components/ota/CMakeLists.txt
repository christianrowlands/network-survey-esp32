# ota_sm.c is pure C (host tested in test/host/ota).
idf_component_register(
    SRCS "ota.c" "ota_confirm.c" "ota_sm.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES app_update esp_app_format esp_timer gatt_server link mbedtls ns_proto
)
