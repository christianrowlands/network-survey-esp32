#include "ui_io.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_NS_BOARD_LED_ADDRESSABLE
#include "led_strip.h"
#endif

static const char *TAG = "ui_io";

#define TICK_MS 20
#define DEBOUNCE_TICKS 2
#define HOLD_MS 3000
#define WIPE_HOLD_MS 10000
#define BOOT_WINDOW_MS 5000
#define LED_LEVEL 24  // RGB brightness, 0 to 255

#if CONFIG_NS_BOARD_LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#else
#define LED_ACTIVE_LOW 0
#endif

// One pattern repeats every step_ms * steps; bit i of mask lights step i.
typedef struct {
    uint16_t step_ms;
    uint8_t steps;
    uint32_t mask;
    uint8_t r, g, b;
} led_pattern_t;

static const led_pattern_t PATTERNS[UI_LED_MODE_COUNT] = {
    [UI_LED_IDLE] = {100, 20, 0x1, 0, 0, 1},          // short blue blink every 2 s
    [UI_LED_CONNECTED] = {100, 1, 0x1, 0, 0, 1},      // solid blue
    [UI_LED_SURVEYING] = {100, 10, 0x1F, 0, 1, 0},    // green, 1 Hz
    [UI_LED_PAIRING] = {100, 2, 0x1, 0, 1, 1},        // cyan, fast blink
    [UI_LED_OTA] = {100, 4, 0x3, 1, 0, 1},            // magenta, 2.5 Hz
    [UI_LED_ERROR] = {100, 10, 0x5, 1, 0, 0},         // red double blink
    [UI_LED_CONFIRM] = {50, 2, 0x1, 1, 1, 1},         // white, rapid flash
};

static volatile uint32_t s_active = 1u << UI_LED_IDLE;
static volatile int64_t s_pulse_until_ms[UI_LED_MODE_COUNT];
static ui_button_cb_t s_button_cb;
static void *s_button_ctx;

#if CONFIG_NS_BOARD_LED_ADDRESSABLE
static led_strip_handle_t s_strip;
#endif

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void ui_io_led_set(ui_led_mode_t mode, bool active)
{
    if (mode <= UI_LED_IDLE || mode >= UI_LED_MODE_COUNT) {
        return;
    }
    if (active) {
        __atomic_fetch_or(&s_active, 1u << mode, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_and(&s_active, ~(1u << mode), __ATOMIC_RELAXED);
    }
}

void ui_io_led_pulse(ui_led_mode_t mode, uint32_t duration_ms)
{
    if (mode <= UI_LED_IDLE || mode >= UI_LED_MODE_COUNT) {
        return;
    }
    s_pulse_until_ms[mode] = now_ms() + duration_ms;
    ui_io_led_set(mode, true);
}

const char *ui_button_event_name(ui_button_event_t event)
{
    switch (event) {
    case UI_BUTTON_SHORT_PRESS: return "short press";
    case UI_BUTTON_HOLD_3S: return "hold 3 s";
    case UI_BUTTON_HOLD_10S_AT_BOOT: return "hold 10 s at boot";
    }
    return "?";
}

static esp_err_t led_init(void)
{
#if CONFIG_NS_BOARD_LED_ADDRESSABLE
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_NS_BOARD_LED_GPIO_NUM,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err == ESP_OK) {
        led_strip_clear(s_strip);
    }
    ESP_LOGI(TAG, "LED: addressable RGB on GPIO%d (%s)", CONFIG_NS_BOARD_LED_GPIO_NUM, esp_err_to_name(err));
    return err;
#elif CONFIG_NS_BOARD_LED_GPIO
    gpio_reset_pin(CONFIG_NS_BOARD_LED_GPIO_NUM);
    gpio_set_direction(CONFIG_NS_BOARD_LED_GPIO_NUM, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_NS_BOARD_LED_GPIO_NUM, LED_ACTIVE_LOW);
    ESP_LOGI(TAG, "LED: plain on GPIO%d%s", CONFIG_NS_BOARD_LED_GPIO_NUM,
             LED_ACTIVE_LOW ? " (active low)" : "");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "LED: none");
    return ESP_OK;
#endif
}

static void led_write(const led_pattern_t *p, bool on)
{
#if CONFIG_NS_BOARD_LED_ADDRESSABLE
    if (s_strip == NULL) {
        return;
    }
    if (on) {
        led_strip_set_pixel(s_strip, 0, p->r * LED_LEVEL, p->g * LED_LEVEL, p->b * LED_LEVEL);
        led_strip_refresh(s_strip);
    } else {
        led_strip_clear(s_strip);
    }
#elif CONFIG_NS_BOARD_LED_GPIO
    (void)p;
    gpio_set_level(CONFIG_NS_BOARD_LED_GPIO_NUM, on != LED_ACTIVE_LOW);
#else
    (void)p;
    (void)on;
#endif
}

static void led_tick(int64_t t)
{
    static int last_mode = -1;
    static int last_on = -1;

    for (int m = UI_LED_IDLE + 1; m < UI_LED_MODE_COUNT; m++) {
        if (s_pulse_until_ms[m] != 0 && t >= s_pulse_until_ms[m]) {
            s_pulse_until_ms[m] = 0;
            ui_io_led_set((ui_led_mode_t)m, false);
        }
    }

    uint32_t active = __atomic_load_n(&s_active, __ATOMIC_RELAXED) | (1u << UI_LED_IDLE);
    int mode = 31 - __builtin_clz(active);
    const led_pattern_t *p = &PATTERNS[mode];
    int step = (int)((t / p->step_ms) % p->steps);
    int on = (p->mask >> step) & 1;
    if (mode != last_mode || on != last_on) {
        led_write(p, on);
        last_mode = mode;
        last_on = on;
    }
}

static void emit(ui_button_event_t event)
{
    ESP_LOGI(TAG, "button: %s", ui_button_event_name(event));
    if (s_button_cb != NULL) {
        s_button_cb(event, s_button_ctx);
    }
}

static void button_tick(int64_t t)
{
#if CONFIG_NS_BOARD_BUTTON_GPIO >= 0
    static bool pressed;
    static int same_reads;
    static int64_t press_start;
    static bool in_boot_window, hold_fired, wipe_fired;

    bool level_pressed = gpio_get_level(CONFIG_NS_BOARD_BUTTON_GPIO) == 0;
    same_reads = (level_pressed != pressed) ? same_reads + 1 : 0;
    if (same_reads >= DEBOUNCE_TICKS) {
        same_reads = 0;
        pressed = level_pressed;
        if (pressed) {
            press_start = t;
            in_boot_window = t < BOOT_WINDOW_MS;
            hold_fired = wipe_fired = false;
        } else if (!hold_fired) {
            emit(UI_BUTTON_SHORT_PRESS);
        }
    }
    if (pressed) {
        int64_t held = t - press_start;
        if (!hold_fired && held >= HOLD_MS) {
            hold_fired = true;
            emit(UI_BUTTON_HOLD_3S);
        }
        if (in_boot_window && !wipe_fired && held >= WIPE_HOLD_MS) {
            wipe_fired = true;
            emit(UI_BUTTON_HOLD_10S_AT_BOOT);
        }
    }
#else
    (void)t;
#endif
}

static void ui_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        int64_t t = now_ms();
        button_tick(t);
        led_tick(t);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t ui_io_init(ui_button_cb_t button_cb, void *ctx)
{
    s_button_cb = button_cb;
    s_button_ctx = ctx;
    esp_err_t err = led_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED init failed, continuing without LED");
    }
#if CONFIG_NS_BOARD_BUTTON_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_NS_BOARD_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_LOGI(TAG, "button: BOOT on GPIO%d", CONFIG_NS_BOARD_BUTTON_GPIO);
#endif
    return xTaskCreate(ui_task, "ui_io", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
