// Network Survey board firmware entry point.

#include "ble_sniffer.h"
#include "board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gatt_server.h"
#include "gps.h"
#include "link.h"
#include "nvs_flash.h"
#include "ota.h"
#include "pairing_cmds.h"
#include "sniffer.h"
#include "ui_io.h"

static const char *TAG = "main";

static void on_button(ui_button_event_t event, void *ctx)
{
    (void)ctx;
    pairing_cmds_on_button(event);
}

static void on_gatt(gatt_event_t event, uint16_t conn_handle, void *ctx)
{
    (void)conn_handle;
    (void)ctx;
    switch (event) {
    case GATT_EVENT_CONNECTED:
    case GATT_EVENT_DISCONNECTED:
        ui_io_led_set(UI_LED_CONNECTED, gatt_server_connection_count() > 0);
        break;
    case GATT_EVENT_ENCRYPTED:
        break;
    case GATT_EVENT_PAIRING_CHANGED:
    case GATT_EVENT_BONDS_WIPED:
        pairing_cmds_on_gatt(event);
        break;
    }
}

// Capture backend for the link layer: records from the Wi-Fi aggregator, the
// BLE sniffer and the GPS go to the link sink, raw frames to the Raw sink;
// Control's Configure, Start and Stop drive the capture modules.
static void on_record(const ns_board_v1_Record *rec, void *ctx)
{
    (void)ctx;
    link_submit_record(rec);
}

static const aggregator_sink_t s_sink = {.record = on_record};

static void on_raw(const ns_board_v1_RawFrame *frame, void *ctx)
{
    (void)ctx;
    link_submit_raw(frame);
}

static uint32_t raw_snaplen(void *ctx)
{
    (void)ctx;
    return link_raw_wanted() ? link_raw_snaplen() : 0;
}

static void on_raw_dropped(uint32_t count, void *ctx)
{
    (void)ctx;
    link_count_raw_dropped(count);
}

static const sniffer_raw_sink_t s_raw_sink = {.frame = on_raw, .snaplen = raw_snaplen, .dropped = on_raw_dropped};

// GPS fix state goes out in Status at once; a detected GPS also sets the
// advertising flag.
static void on_gps_changed(void *ctx)
{
    (void)ctx;
    static bool advertised;
    link_status_changed();
    if (board_gps_present() != advertised) {
        advertised = board_gps_present();
        gatt_server_refresh_advertising();
    }
}

static const gps_sink_t s_gps_sink = {.record = on_record, .changed = on_gps_changed};

static esp_err_t survey_configure(const ns_board_v1_Configure *cfg, ns_board_v1_ConfigureResult *result)
{
    if (sniffer_configure(cfg, result) != ns_board_v1_ResultCode_RESULT_CODE_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    ble_sniffer_configure(cfg);
    return ESP_OK;
}

static esp_err_t survey_start(void)
{
    esp_err_t err = sniffer_start();
    if (err != ESP_OK) {
        return err;  // nothing else started
    }
    ble_sniffer_start();
    gps_set_active(true);
    ui_io_led_set(UI_LED_SURVEYING, true);
    return ESP_OK;
}

// Every stop goes through here: Stop, OTA begin and the orphan stop.
static esp_err_t survey_stop(void)
{
    ui_io_led_set(UI_LED_SURVEYING, false);
    gps_set_active(false);
    ble_sniffer_stop();
    return sniffer_stop();
}

static const link_survey_ops_t s_survey = {
    .configure = survey_configure,
    .start = survey_start,
    .stop = survey_stop,
};

static void fill_status(ns_board_v1_BoardStatus *st)
{
    static uint32_t last_rx;
    static uint64_t last_ms;
    static uint32_t rx_per_sec;
    sniffer_stats_t stats;
    sniffer_get_stats(&stats);
    uint64_t now = link_mono_ms();
    if (now - last_ms >= 1000) {
        rx_per_sec = last_ms == 0 ? 0 : (uint32_t)((uint64_t)(stats.rx_frames - last_rx) * 1000 / (now - last_ms));
        last_rx = stats.rx_frames;
        last_ms = now;
    }
    st->current_channel = sniffer_current_channel();
    st->rx_frames_per_sec = rx_per_sec;
    // Frames lost to a full receive queue or a full aggregator table, and BLE
    // advertising reports lost to a full report queue.
    ble_sniffer_stats_t ble;
    ble_sniffer_get_stats(&ble);
    st->dropped_rx_frames = stats.dropped_rx_frames + stats.agg_dropped + ble.dropped_reports;
    st->gps_state = gps_state();
}

static void on_ota_active(bool active)
{
    ui_io_led_set(UI_LED_OTA, active);
}

// Self-test for a freshly installed image: capture and link initialised, the
// sniffer task running and the BLE host synced (the advertised name is set).
static bool s_started_ok;

static bool self_test(void)
{
    TaskHandle_t sniffer = xTaskGetHandle("sniffer");  // task name in sniffer.c
    return s_started_ok && sniffer != NULL && eTaskGetState(sniffer) != eDeleted &&
           gatt_server_short_id()[0] != '\0';
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Network Survey board %s on %s (%s)", app->version, board_model_name(), board_chip_name());

    init_nvs();
    ESP_ERROR_CHECK(ui_io_init(on_button, NULL));

#if CONFIG_NS_SNIFFER_DEBUG_AUTOSTART
    // Bring-up only (sniffer Kconfig): capture from boot and log per-dwell
    // counts instead of sending records.
    esp_err_t capture = sniffer_debug_autostart();
    (void)s_sink;
    (void)s_raw_sink;
#else
    esp_err_t capture = sniffer_init(&s_sink);
    sniffer_set_raw_sink(&s_raw_sink);
#endif
    if (capture != ESP_OK) {
        ESP_LOGE(TAG, "capture unavailable: %s", esp_err_to_name(capture));
        ui_io_led_set(UI_LED_ERROR, true);
    }
    pairing_cmds_register();
    // Before the BLE host starts, so a configured but unusable GPS is cleared
    // before the first advertisement.
    esp_err_t gps = gps_init(&s_gps_sink);
    // OTA first: a new image must reach its rollback checks even when the
    // rest fails to start.
    esp_err_t ota = ota_init(on_ota_active);
    esp_err_t link = link_init(capture == ESP_OK ? &s_survey : NULL, capture == ESP_OK ? fill_status : NULL);
    if (ota != ESP_OK || link != ESP_OK || gatt_server_init(on_gatt, NULL) != ESP_OK) {
        ui_io_led_set(UI_LED_ERROR, true);
    } else {
        s_started_ok = capture == ESP_OK;
    }
    // BLE sniffing and the GPS are optional: the board still surveys Wi-Fi
    // without them.
    esp_err_t ble = ble_sniffer_init(on_record, NULL);
    if (ble != ESP_OK || gps != ESP_OK) {
        ESP_LOGE(TAG, "BLE sniffer: %s, GPS: %s", esp_err_to_name(ble), esp_err_to_name(gps));
    }
    ota_start_self_test(self_test);
}
