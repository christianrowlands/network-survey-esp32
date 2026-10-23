// Status LED patterns and BOOT button events.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// LED modes in rising priority. Several can be active at once; the LED shows
// the highest active one. IDLE is always active.
typedef enum {
    UI_LED_IDLE = 0,
    UI_LED_CONNECTED,
    UI_LED_SURVEYING,
    UI_LED_PAIRING,
    UI_LED_OTA,
    UI_LED_ERROR,
    UI_LED_CONFIRM,  // short acknowledgement, shown with ui_io_led_pulse
    UI_LED_MODE_COUNT,
} ui_led_mode_t;

typedef enum {
    UI_BUTTON_SHORT_PRESS,       // released before 3 s
    UI_BUTTON_HOLD_3S,           // still held after 3 s (fires once per press)
    UI_BUTTON_HOLD_10S_AT_BOOT,  // pressed in the first 5 s after boot and held 10 s
} ui_button_event_t;

// Called from the ui_io task. Keep it short; post work elsewhere if needed.
typedef void (*ui_button_cb_t)(ui_button_event_t event, void *ctx);

esp_err_t ui_io_init(ui_button_cb_t button_cb, void *ctx);

void ui_io_led_set(ui_led_mode_t mode, bool active);

// Shows mode for duration_ms, then clears it.
void ui_io_led_pulse(ui_led_mode_t mode, uint32_t duration_ms);

const char *ui_button_event_name(ui_button_event_t event);
