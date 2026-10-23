// Board clock, the record and raw rings, and the sinks capture code calls.
// Producers hold the ring mutex only to trim, encode and copy one entry, so
// they never wait for BLE.

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "link_frame.h"
#include "link_internal.h"
#include "link_ring.h"
#include "ns_proto_fit.h"
#include "pb_encode.h"

static const char *TAG = "link";

#define RING_PSRAM (1024 * 1024)
#define RING_INTERNAL (24 * 1024)
#define RAW_RING_PSRAM (128 * 1024)
#define RAW_RING_INTERNAL (6 * 1024)
#define DEFAULT_FIT_MTU 185
// Entries a build call may drop under the lock before it lets producers in.
#define MAX_DISCARDS 16
// RawFrame fields other than data take at most this many bytes encoded.
#define RAW_OVERHEAD 64

#define TAG_SYNCED 0x01  // ring entry tag bit: record times are Unix ms

static SemaphoreHandle_t s_lock;
static link_ring_t s_rec_ring;
static link_ring_t s_raw_ring;
static uint8_t s_buf[LINK_FRAME_MAX];  // encode and copy buffer, under s_lock
static uint32_t s_produced;
static uint32_t s_produced_seen;
static uint32_t s_rec_lost;  // too large to queue or send, or lost after the ring
static uint32_t s_raw_lost;
static volatile uint32_t s_raw_lost_ext;  // link_count_raw_dropped, one caller task
static volatile uint16_t s_fit_mtu = DEFAULT_FIT_MTU;

static portMUX_TYPE s_clock_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_offset_ms;
static bool s_time_synced;

// ---------------------------------------------------------------------------
// Clock

uint64_t link_mono_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

uint64_t link_board_time_ms(uint64_t mono_ms, bool *synced)
{
    taskENTER_CRITICAL(&s_clock_mux);
    int64_t offset = s_offset_ms;
    bool is_synced = s_time_synced;
    taskEXIT_CRITICAL(&s_clock_mux);
    if (synced != NULL) {
        *synced = is_synced;
    }
    return is_synced ? (uint64_t)((int64_t)mono_ms + offset) : mono_ms;
}

void link_clock_set_unix(uint64_t unix_ms)
{
    int64_t offset = (int64_t)unix_ms - (int64_t)link_mono_ms();
    taskENTER_CRITICAL(&s_clock_mux);
    s_offset_ms = offset;
    s_time_synced = true;
    taskEXIT_CRITICAL(&s_clock_mux);
}

// ---------------------------------------------------------------------------
// Sinks (any task)

static uint64_t *record_time(ns_board_v1_Record *rec)
{
    switch (rec->which_kind) {
    case ns_board_v1_Record_ap_tag:
        return &rec->kind.ap.board_time_ms;
    case ns_board_v1_Record_client_tag:
        return &rec->kind.client.board_time_ms;
    case ns_board_v1_Record_probe_tag:
        return &rec->kind.probe.board_time_ms;
    case ns_board_v1_Record_ble_tag:
        return &rec->kind.ble.board_time_ms;
    case ns_board_v1_Record_gps_tag:
        return &rec->kind.gps.board_time_ms;
    default:
        return NULL;
    }
}

// Encodes msg and queues it. Caller holds s_lock.
static bool push_locked(link_ring_t *ring, uint8_t tag, const pb_msgdesc_t *fields, const void *msg)
{
    pb_ostream_t os = pb_ostream_from_buffer(s_buf, sizeof(s_buf));
    if (!pb_encode(&os, fields, msg)) {
        return false;
    }
    return link_ring_push(ring, tag, s_buf, os.bytes_written) >= 0;
}

bool link_submit_record(const ns_board_v1_Record *rec)
{
    static ns_board_v1_Record copy;  // under s_lock
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_produced++;
    copy = *rec;
    uint64_t *t = record_time(&copy);
    bool ok = false;
    if (t != NULL) {
        bool synced;
        *t = link_board_time_ms(*t, &synced);
        ok = ns_proto_fit_record(&copy, link_batch_max_record(link_frame_payload(s_fit_mtu))) &&
             push_locked(&s_rec_ring, synced ? TAG_SYNCED : 0, ns_board_v1_Record_fields, &copy);
    }
    if (!ok) {
        s_rec_lost++;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool link_submit_raw(const ns_board_v1_RawFrame *frame)
{
    static ns_board_v1_RawFrame copy;  // under s_lock
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy = *frame;
    bool synced;
    uint64_t mono_ms = copy.board_time_us / 1000;
    copy.board_time_us += (link_board_time_ms(mono_ms, &synced) - mono_ms) * 1000;
    copy.time_synced = synced;
    bool ok = push_locked(&s_raw_ring, 0, ns_board_v1_RawFrame_fields, &copy);
    if (!ok) {
        s_raw_lost++;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void link_count_raw_dropped(uint32_t n)
{
    s_raw_lost_ext += n;
}

static volatile bool s_raw_listener;

void link_sink_set_raw_listener(bool listening)
{
    s_raw_listener = listening;
}

bool link_raw_wanted(void)
{
    return s_raw_listener && link_control_raw_enabled();
}

void link_sink_set_fit_mtu(uint16_t mtu)
{
    s_fit_mtu = mtu;
}

uint32_t link_raw_snaplen(void)
{
    ns_board_v1_Configure cfg;
    link_get_config(&cfg);
    size_t room = link_frame_max_message(link_frame_payload(s_fit_mtu));
    uint32_t fit = room > RAW_OVERHEAD ? (uint32_t)(room - RAW_OVERHEAD) : 0;
    uint32_t snap = 512;
    if (cfg.raw_snaplen != 0 && cfg.raw_snaplen < snap) {
        snap = cfg.raw_snaplen;
    }
    return snap < fit ? snap : fit;
}

// ---------------------------------------------------------------------------
// Consumer side (link task)

size_t link_sink_build_records(uint8_t *buf, size_t payload, int *count)
{
    size_t max = link_batch_max_record(payload);
    link_batch_t b;
    bool started = false;
    uint8_t tag;
    size_t len;
    int discards = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    while (link_ring_peek(&s_rec_ring, &tag, &len)) {
        if (len > max) {  // queued for a larger MTU than this connection's
            link_ring_discard(&s_rec_ring);
            s_rec_lost++;
            if (++discards >= MAX_DISCARDS) {
                break;  // keep the lock short; the next call goes on
            }
            continue;
        }
        bool synced = (tag & TAG_SYNCED) != 0;
        if (!started) {
            link_batch_init(&b, buf, payload, 0, synced);
            started = true;
        } else if (synced != b.time_synced || !link_batch_can_add(&b, len)) {
            break;
        }
        link_ring_pop(&s_rec_ring, s_buf, sizeof(s_buf), &tag);
        link_batch_add(&b, s_buf, len);
    }
    xSemaphoreGive(s_lock);
    *count = started ? b.count : 0;
    return started ? link_batch_finish(&b) : 0;
}

size_t link_sink_build_raw(uint8_t *buf, size_t payload, int *count)
{
    size_t max = link_frame_max_message(payload);
    link_frame_t f;
    link_frame_init(&f, buf, payload, 0);
    uint8_t tag;
    size_t len;
    int discards = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    while (link_ring_peek(&s_raw_ring, &tag, &len)) {
        if (len > max) {
            link_ring_discard(&s_raw_ring);
            s_raw_lost++;
            if (++discards >= MAX_DISCARDS) {
                break;
            }
            continue;
        }
        if (f.len + link_varint_size((uint32_t)len) + len > payload) {
            break;
        }
        link_ring_pop(&s_raw_ring, s_buf, sizeof(s_buf), &tag);
        link_frame_add(&f, s_buf, len);
    }
    xSemaphoreGive(s_lock);
    *count = f.count;
    return f.count > 0 ? f.len : 0;
}

bool link_sink_records_empty(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool empty = s_rec_ring.entries == 0;
    xSemaphoreGive(s_lock);
    return empty;
}

void link_sink_clear_raw(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    link_ring_clear(&s_raw_ring);
    xSemaphoreGive(s_lock);
}

void link_sink_count_lost(bool raw, uint32_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (raw) {
        s_raw_lost += n;
    } else {
        s_rec_lost += n;
    }
    xSemaphoreGive(s_lock);
}

void link_sink_fill_status(ns_board_v1_BoardStatus *status)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    status->ring_fill_percent = link_ring_fill_percent(&s_rec_ring);
    status->dropped_records = s_rec_ring.dropped + s_rec_lost;
    status->dropped_raw_frames = s_raw_ring.dropped + s_raw_lost + s_raw_lost_ext;
    xSemaphoreGive(s_lock);
}

uint32_t link_sink_take_produced(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t n = s_produced - s_produced_seen;
    s_produced_seen = s_produced;
    xSemaphoreGive(s_lock);
    return n;
}

static uint8_t *alloc_ring(size_t psram, size_t internal, size_t *size)
{
    uint8_t *buf = heap_caps_malloc(psram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    *size = psram;
    if (buf == NULL) {
        buf = heap_caps_malloc(internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        *size = internal;
    }
    return buf;
}

esp_err_t link_sink_init(void)
{
    size_t rec_size, raw_size;
    uint8_t *rec_buf = alloc_ring(RING_PSRAM, RING_INTERNAL, &rec_size);
    uint8_t *raw_buf = alloc_ring(RAW_RING_PSRAM, RAW_RING_INTERNAL, &raw_size);
    s_lock = xSemaphoreCreateMutex();
    if (rec_buf == NULL || raw_buf == NULL || s_lock == NULL) {
        ESP_LOGE(TAG, "no memory for the rings");
        return ESP_ERR_NO_MEM;
    }
    link_ring_init(&s_rec_ring, rec_buf, rec_size);
    link_ring_init(&s_raw_ring, raw_buf, raw_size);
    ESP_LOGI(TAG, "record ring %u KB, raw ring %u KB (%s)", (unsigned)(rec_size / 1024),
             (unsigned)(raw_size / 1024), rec_size == RING_PSRAM ? "PSRAM" : "internal RAM");
    return ESP_OK;
}
