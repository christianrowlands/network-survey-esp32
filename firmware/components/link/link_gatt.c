// GATT callbacks (NimBLE host task). They queue events for the link task and
// keep Control writes in a few request slots until the link task takes them.

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "gatt_server.h"
#include "host/ble_hs.h"
#include "link_internal.h"
#include "pb_encode.h"

static const char *TAG = "link";

#define CTL_SLOTS 4
#define EVENT_DEPTH 24

typedef struct {
    volatile bool used;
    volatile bool ready;
    volatile bool taken;  // being copied out by the link task
    uint16_t conn;
    uint16_t len;
    uint32_t order;
    uint8_t data[LINK_CTL_MAX];
} ctl_slot_t;

static QueueHandle_t s_events;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;  // Control slots
static ctl_slot_t s_slots[CTL_SLOTS];
static uint32_t s_slot_order;

static link_ota_write_t s_ota_write;

static void post(const link_event_t *ev)
{
    if (xQueueSend(s_events, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, event %d lost", ev->type);
    }
}

static void free_slots_of(uint16_t conn)
{
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CTL_SLOTS; i++) {
        if (s_slots[i].used && !s_slots[i].taken && s_slots[i].conn == conn) {
            s_slots[i].used = false;
            s_slots[i].ready = false;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void hook_connected(uint16_t conn)
{
    post(&(link_event_t){.type = LINK_EV_CONNECTED, .conn = conn});
}

static void hook_disconnected(uint16_t conn)
{
    free_slots_of(conn);
    post(&(link_event_t){.type = LINK_EV_DISCONNECTED, .conn = conn});
}

static void hook_subscribed(uint16_t conn, gatt_chr_t chr, bool enabled)
{
    post(&(link_event_t){.type = LINK_EV_SUBSCRIBED, .conn = conn, .chr = (uint8_t)chr, .enabled = enabled});
}

static void hook_indicate_done(uint16_t conn, gatt_chr_t chr, int status)
{
    post(&(link_event_t){.type = LINK_EV_INDICATE_DONE, .conn = conn, .chr = (uint8_t)chr, .status = (int16_t)status});
}

static int hook_write(uint16_t conn, gatt_chr_t chr, const uint8_t *data, uint16_t len)
{
    if (chr == GATT_CHR_OTA) {
        if (s_ota_write != NULL) {
            s_ota_write(conn, data, len);
        }
        return 0;
    }
    if (chr != GATT_CHR_CONTROL || len > LINK_CTL_MAX) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    ctl_slot_t *slot = NULL;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CTL_SLOTS && slot == NULL; i++) {
        if (!s_slots[i].used) {
            slot = &s_slots[i];
            slot->used = true;
            slot->ready = false;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (slot == NULL) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    slot->conn = conn;
    slot->len = len;
    memcpy(slot->data, data, len);
    taskENTER_CRITICAL(&s_mux);
    slot->order = ++s_slot_order;
    slot->ready = true;
    taskEXIT_CRITICAL(&s_mux);
    post(&(link_event_t){.type = LINK_EV_WAKE});
    return 0;
}

static const gatt_link_hooks_t s_hooks = {
    .connected = hook_connected,
    .disconnected = hook_disconnected,
    .subscribed = hook_subscribed,
    .indicate_done = hook_indicate_done,
    .write = hook_write,
};

void link_set_ota_write_handler(link_ota_write_t handler)
{
    s_ota_write = handler;
}

bool link_send_ota_ack(uint16_t conn, const ns_board_v1_OtaAck *ack)
{
    link_event_t ev = {.type = LINK_EV_OTA_ACK, .conn = conn};
    pb_ostream_t os = pb_ostream_from_buffer(ev.data, sizeof(ev.data));
    if (!pb_encode(&os, ns_board_v1_OtaAck_fields, ack)) {
        return false;
    }
    ev.len = (uint8_t)os.bytes_written;
    return xQueueSend(s_events, &ev, 0) == pdTRUE;
}

// Oldest ready Control request of conn, copied out and released.
bool link_gatt_take_request(uint16_t conn, uint8_t *out, size_t *len)
{
    taskENTER_CRITICAL(&s_mux);
    ctl_slot_t *best = NULL;
    for (int i = 0; i < CTL_SLOTS; i++) {
        ctl_slot_t *s = &s_slots[i];
        if (s->used && s->ready && s->conn == conn && (best == NULL || s->order < best->order)) {
            best = s;
        }
    }
    if (best != NULL) {
        best->ready = false;
        best->taken = true;  // keeps the slot from being freed or reused
    }
    taskEXIT_CRITICAL(&s_mux);
    if (best == NULL) {
        return false;
    }
    // Copy with interrupts on, then release the slot.
    *len = best->len;
    memcpy(out, best->data, best->len);
    taskENTER_CRITICAL(&s_mux);
    best->taken = false;
    best->used = false;
    taskEXIT_CRITICAL(&s_mux);
    return true;
}

bool link_gatt_next_event(link_event_t *ev, TickType_t wait)
{
    return xQueueReceive(s_events, ev, wait) == pdTRUE;
}

esp_err_t link_gatt_init(void)
{
    s_events = xQueueCreate(EVENT_DEPTH, sizeof(link_event_t));
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    gatt_server_set_link_hooks(&s_hooks);
    return ESP_OK;
}
