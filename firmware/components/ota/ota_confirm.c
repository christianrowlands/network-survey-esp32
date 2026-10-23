// Confirmation of a freshly installed image (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
// The first boot of an OTA image is PENDING_VERIFY; the image is confirmed by
// a compatible Hello while the self-test passes, by OtaBegin (an update cannot
// start from a pending image), or by the self-test after 60 s of uptime. A
// restart before that returns to the previous image.

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ota.h"
#include "ota_internal.h"

static const char *TAG = "ota";

#define SELF_TEST_AFTER_US (60 * 1000 * 1000LL)
#define SELF_TEST_GIVE_UP_US (180 * 1000 * 1000LL)
#define SELF_TEST_PERIOD_US (5 * 1000 * 1000)

// Held across esp_ota_mark_app_valid_cancel_rollback, so a caller that finds
// the image already confirmed knows the flash write is done.
static SemaphoreHandle_t s_lock;
static volatile bool s_pending;  // running image is PENDING_VERIFY
static bool s_confirmed;         // it was, and is confirmed now
static bool s_reported;          // a HelloResult carried ota_image_confirmed
static bool (*volatile s_healthy)(void);
static esp_timer_handle_t s_timer;

void ota_confirm_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    s_pending = esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    ESP_LOGI(TAG, "running %s from %s%s", esp_app_get_description()->version, running->label,
             s_pending ? ", pending verification" : "");
}

bool ota_image_pending(void)
{
    return s_pending;
}

bool ota_confirm(const char *why)
{
    if (s_lock == NULL || !s_pending) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool done = false;
    if (s_pending) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            s_pending = false;
            s_confirmed = true;
            done = true;
            ESP_LOGI(TAG, "image confirmed (%s)", why);
        } else {
            ESP_LOGE(TAG, "confirming the image failed: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_lock);
    return done;
}

bool ota_confirm_on_hello(void)
{
    // A Hello only shows that BLE works; the rest must be up too, else the
    // image waits for the 60 s self-test (or rolls back).
    bool (*healthy)(void) = s_healthy;
    if (s_pending && healthy != NULL && healthy()) {
        ota_confirm("Hello");
    }
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool report = s_confirmed && !s_reported;
    s_reported = s_reported || report;
    xSemaphoreGive(s_lock);
    return report;
}

static void self_test(void *arg)
{
    (void)arg;
    int64_t uptime = esp_timer_get_time();
    if (!s_pending) {
        esp_timer_stop(s_timer);
        return;
    }
    if (uptime < SELF_TEST_AFTER_US) {
        return;
    }
    if (s_healthy()) {
        ota_confirm("self-test");
        esp_timer_stop(s_timer);
    } else if (uptime >= SELF_TEST_GIVE_UP_US) {
        esp_timer_stop(s_timer);
        ESP_LOGE(TAG, "self-test failed, returning to the previous image");
        // Returns only when there is nothing to return to; keep this image.
        esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "rollback failed (%s), keeping this image", esp_err_to_name(err));
        ota_confirm("no rollback target");
    }
}

void ota_start_self_test(bool (*healthy)(void))
{
    s_healthy = healthy;
    if (!s_pending || healthy == NULL || s_timer != NULL) {
        return;
    }
    const esp_timer_create_args_t args = {.callback = self_test, .name = "ota_selftest"};
    if (esp_timer_create(&args, &s_timer) != ESP_OK || esp_timer_start_periodic(s_timer, SELF_TEST_PERIOD_US) != ESP_OK) {
        ESP_LOGE(TAG, "self-test timer failed; the image waits for a Hello");
    }
}
