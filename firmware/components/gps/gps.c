#include "gps.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nmea.h"
#include "sdkconfig.h"

static const char *TAG = "gps";

#if !CONFIG_NS_BOARD_GPS_NONE

#define GPS_UART UART_NUM_1
#define RX_BUF 1024
#define TASK_STACK 3072
#define TASK_PRIO 3
#define READ_TIMEOUT_MS 200
#define FIX_STALE_MS 3000

static gps_sink_t s_sink;
static volatile bool s_active;
static volatile ns_board_v1_GpsState s_state = ns_board_v1_GpsState_GPS_STATE_NOT_PRESENT;
static nmea_line_t s_line;
static nmea_merge_t s_merge;
static uint64_t s_last_fix_ms;
static bool s_has_fix;

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void notify(void)
{
    if (s_sink.changed != NULL) {
        s_sink.changed(s_sink.ctx);
    }
}

static void on_fix(const ns_board_v1_GpsFix *fix, void *ctx)
{
    (void)ctx;
    s_last_fix_ms = fix->board_time_ms;
    s_has_fix = true;
    if (s_active && s_sink.record != NULL) {
        ns_board_v1_Record rec = ns_board_v1_Record_init_zero;
        rec.which_kind = ns_board_v1_Record_gps_tag;
        rec.kind.gps = *fix;
        s_sink.record(&rec, s_sink.ctx);
    }
}

static void update_state(bool present, uint64_t now)
{
    ns_board_v1_GpsState st = ns_board_v1_GpsState_GPS_STATE_NOT_PRESENT;
    if (present) {
        st = s_has_fix && now - s_last_fix_ms <= FIX_STALE_MS ? ns_board_v1_GpsState_GPS_STATE_FIX
                                                                : ns_board_v1_GpsState_GPS_STATE_NO_FIX;
    }
    if (st != s_state) {
        s_state = st;
        ESP_LOGI(TAG, "%s", st == ns_board_v1_GpsState_GPS_STATE_FIX      ? "fix"
                           : st == ns_board_v1_GpsState_GPS_STATE_NO_FIX ? "no fix"
                                                                          : "not present");
        notify();
    }
}

static void gps_task(void *arg)
{
    (void)arg;
    static uint8_t buf[128];
    bool present = board_gps_present();  // fitted, or detected below
#if CONFIG_NS_BOARD_GPS_AUTO
    uint64_t start = now_ms();
#endif
    for (;;) {
        int n = uart_read_bytes(GPS_UART, buf, sizeof(buf), pdMS_TO_TICKS(READ_TIMEOUT_MS));
        uint64_t now = now_ms();
        for (int i = 0; i < n; i++) {
            if (!nmea_line_feed(&s_line, buf[i])) {
                continue;
            }
            nmea_sentence_t s;
            if (nmea_parse(s_line.buf, &s) == NMEA_INVALID) {
                continue;
            }
            if (!present) {
                present = true;
                board_set_gps_present(true);
                ESP_LOGI(TAG, "receiver detected on GPIO%d", CONFIG_NS_BOARD_GPS_RX_GPIO);
                notify();
            }
            nmea_merge_add(&s_merge, &s, now);
        }
        nmea_merge_poll(&s_merge, now);
#if CONFIG_NS_BOARD_GPS_AUTO
        if (!present && now - start >= CONFIG_NS_BOARD_GPS_DETECT_S * 1000ull) {
            ESP_LOGI(TAG, "no NMEA within %d s, GPS off", CONFIG_NS_BOARD_GPS_DETECT_S);
            uart_driver_delete(GPS_UART);
            gpio_pullup_dis(CONFIG_NS_BOARD_GPS_RX_GPIO);
            vTaskDelete(NULL);
        }
#endif
        update_state(present, now);
    }
}

esp_err_t gps_init(const gps_sink_t *sink)
{
    if (CONFIG_NS_BOARD_GPS_RX_GPIO < 0) {
        // Not a usable receiver: do not advertise one.
        ESP_LOGE(TAG, "GPS configured without an RX GPIO (NS_BOARD_GPS_RX_GPIO), off");
        board_set_gps_present(false);
        return ESP_OK;
    }
    s_sink = *sink;
    nmea_line_init(&s_line);
    nmea_merge_init(&s_merge, on_fix, NULL);
    const uart_config_t cfg = {
        .baud_rate = CONFIG_NS_BOARD_GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(GPS_UART, RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(GPS_UART, &cfg);
    }
    if (err == ESP_OK) {
        // Receive only: the board never sends to the receiver.
        err = uart_set_pin(GPS_UART, UART_PIN_NO_CHANGE, CONFIG_NS_BOARD_GPS_RX_GPIO, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        gpio_pullup_en(CONFIG_NS_BOARD_GPS_RX_GPIO);  // idle high when nothing is connected
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART setup: %s", esp_err_to_name(err));
        uart_driver_delete(GPS_UART);
        return err;
    }
    if (board_gps_present()) {
        update_state(true, now_ms());
    }
    if (xTaskCreate(gps_task, "gps", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        uart_driver_delete(GPS_UART);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "listening on GPIO%d at %d baud%s", CONFIG_NS_BOARD_GPS_RX_GPIO, CONFIG_NS_BOARD_GPS_BAUD,
             board_gps_present() ? "" : " (detecting)");
    return ESP_OK;
}

void gps_set_active(bool active)
{
    s_active = active;
}

ns_board_v1_GpsState gps_state(void)
{
    return s_state;
}

#else  // no GPS in the board config

esp_err_t gps_init(const gps_sink_t *sink)
{
    (void)sink;
    (void)TAG;
    return ESP_OK;
}

void gps_set_active(bool active)
{
    (void)active;
}

ns_board_v1_GpsState gps_state(void)
{
    return ns_board_v1_GpsState_GPS_STATE_NOT_PRESENT;
}

#endif
