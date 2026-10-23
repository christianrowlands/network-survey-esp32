#include "board.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

ns_board_v1_BoardModel board_model(void)
{
    return (ns_board_v1_BoardModel)CONFIG_NS_BOARD_MODEL;
}

const char *board_model_name(void)
{
    return CONFIG_NS_BOARD_MODEL_NAME;
}

ns_board_v1_Chip board_chip(void)
{
#if CONFIG_IDF_TARGET_ESP32C5
    return ns_board_v1_Chip_CHIP_ESP32_C5;
#elif CONFIG_IDF_TARGET_ESP32C6
    return ns_board_v1_Chip_CHIP_ESP32_C6;
#elif CONFIG_IDF_TARGET_ESP32S3
    return ns_board_v1_Chip_CHIP_ESP32_S3;
#elif CONFIG_IDF_TARGET_ESP32C3
    return ns_board_v1_Chip_CHIP_ESP32_C3;
#else
    return ns_board_v1_Chip_CHIP_OTHER;
#endif
}

const char *board_chip_name(void)
{
    return CONFIG_IDF_TARGET;
}

bool board_has_5ghz(void)
{
#if SOC_WIFI_SUPPORT_5G
    return true;
#else
    return false;
#endif
}

// A fitted GPS is present from boot; a detected one once gps.c found it.
#if CONFIG_NS_BOARD_GPS_FITTED
static volatile bool s_gps_present = true;
#else
static volatile bool s_gps_present;
#endif

bool board_gps_present(void)
{
    return s_gps_present;
}

void board_set_gps_present(bool present)
{
    s_gps_present = present;
}

bool board_has_battery(void)
{
    return CONFIG_NS_BOARD_BATTERY_GPIO >= 0;
}

#if CONFIG_NS_BOARD_BATTERY_GPIO >= 0

static const char *TAG = "board";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_channel;

static esp_err_t battery_init(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(CONFIG_NS_BOARD_BATTERY_GPIO, &unit, &s_channel), TAG,
                        "battery GPIO is not an ADC pin");
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = unit };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "adc unit");
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_channel, &chan_cfg), TAG, "adc channel");
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit, .chan = s_channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        s_cali = NULL;
    }
#endif
#if CONFIG_NS_BOARD_BATTERY_ENABLE_GPIO >= 0
    gpio_reset_pin(CONFIG_NS_BOARD_BATTERY_ENABLE_GPIO);
    gpio_set_direction(CONFIG_NS_BOARD_BATTERY_ENABLE_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_NS_BOARD_BATTERY_ENABLE_GPIO, 0);
#endif
    return ESP_OK;
}

esp_err_t board_battery_mv(uint32_t *out_mv)
{
    if (s_adc == NULL) {
        ESP_RETURN_ON_ERROR(battery_init(), TAG, "battery init");
    }
#if CONFIG_NS_BOARD_BATTERY_ENABLE_GPIO >= 0
    gpio_set_level(CONFIG_NS_BOARD_BATTERY_ENABLE_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
#endif
    int raw = 0, sum_mv = 0;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < 8 && err == ESP_OK; i++) {
        err = adc_oneshot_read(s_adc, s_channel, &raw);
        int mv = raw * 3300 / 4095;  // rough fallback without calibration
        if (err == ESP_OK && s_cali != NULL) {
            err = adc_cali_raw_to_voltage(s_cali, raw, &mv);
        }
        sum_mv += mv;
    }
#if CONFIG_NS_BOARD_BATTERY_ENABLE_GPIO >= 0
    gpio_set_level(CONFIG_NS_BOARD_BATTERY_ENABLE_GPIO, 0);
#endif
    if (err != ESP_OK) {
        return err;
    }
    *out_mv = (uint32_t)(sum_mv / 8) * CONFIG_NS_BOARD_BATTERY_DIVIDER;
    return ESP_OK;
}

#else

esp_err_t board_battery_mv(uint32_t *out_mv)
{
    (void)out_mv;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
