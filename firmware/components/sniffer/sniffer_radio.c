// The Wi-Fi radio for the sniffer: NULL mode, receive-only country config,
// promiscuous filter and tuning. radio_init runs once; the rest is called
// from the sniffer task only.

#include "dot11_parser.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sniffer_internal.h"

static const char *TAG = "sniffer";

static uint8_t s_band;     // 0 unset, 1 = 2.4 GHz, 2 = 5 GHz
static uint32_t s_filter;  // Promiscuous filter mask in effect

esp_err_t radio_init(wifi_promiscuous_cb_t cb)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set_storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_NULL), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    // Receive-only: allow every channel the radio can tune. The board never
    // transmits, so no per-country transmit rules apply.
    wifi_country_t cc = {
        .cc = "01",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
#if CONFIG_SOC_WIFI_SUPPORT_5G
        .wifi_5g_channel_mask = 0x1FFFFFFE,  // Bits 1 to 28: channels 36 to 177
#endif
    };
    err = esp_wifi_set_country(&cc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_country: %s", esp_err_to_name(err));
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_rx_cb(cb), TAG, "set_rx_cb");
    // Every control frame subtype, for raw capture when it asks for control
    // frames (the type filter decides whether any arrive at all).
    wifi_promiscuous_filter_t ctrl = {.filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL};
    ESP_RETURN_ON_ERROR(esp_wifi_set_promiscuous_ctrl_filter(&ctrl), TAG, "set_ctrl_filter");
    return ESP_OK;
}

void radio_set_filter(uint32_t mask)
{
    if (mask == s_filter) {
        return;
    }
    wifi_promiscuous_filter_t filter = {.filter_mask = mask};
    esp_err_t err = esp_wifi_set_promiscuous_filter(&filter);
    if (err == ESP_OK) {
        s_filter = mask;
    } else {
        ESP_LOGW(TAG, "set_promiscuous_filter: %s", esp_err_to_name(err));
    }
}

uint32_t radio_tune(uint8_t channel)
{
    int64_t t0 = esp_timer_get_time();
    uint8_t band = (uint8_t)dot11_channel_band(channel);
#if CONFIG_SOC_WIFI_SUPPORT_5G
    if (band != s_band) {
        wifi_band_mode_t mode = band == 2 ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_2G_ONLY;
        esp_err_t err = esp_wifi_set_band_mode(mode);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set_band_mode(%d): %s", (int)mode, esp_err_to_name(err));
        }
    }
#endif
    s_band = band;
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_channel(%u): %s", channel, esp_err_to_name(err));
    }
    return (uint32_t)(esp_timer_get_time() - t0);
}
