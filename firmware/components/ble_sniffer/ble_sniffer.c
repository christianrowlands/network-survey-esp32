#include "ble_sniffer.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"

static const char *TAG = "ble_sniffer";

#define RING_BYTES (8 * 1024)
#define TABLE_PSRAM 128      // advertisers per window with PSRAM
#define TABLE_INTERNAL 64    // and without
#define TASK_STACK 4096      // the sink (link encode) runs on this task
#define TASK_PRIO 4
#define POLL_MS 100          // checks the scan state at least this often
#define RETRY_MS 1000        // after a failed scan start
#define SUMMARY_MS 60000     // log line while scanning
// NimBLE buffers are shared with the phone's notifications. Below this many
// free ones the scan pauses for the rest of the window so the link can drain.
#define MIN_FREE_MBUFS 8

typedef enum { SCAN_OFF, SCAN_INTERLEAVE, SCAN_CONTINUOUS } scan_mode_t;

// Ring buffer item: this header, then len bytes of AD structures. Fields are
// at most 4 bytes wide (the ring aligns items to 4).
typedef struct {
    uint32_t time_ms;     // low 32 bits of the monotonic clock
    uint8_t addr[6];      // printed order
    uint8_t addr_type;    // ns_board_v1_BleAddressType
    uint8_t adv_type;     // ns_board_v1_BleAdvType
    int8_t rssi;
    int8_t tx_power;
    uint8_t data_status;  // ble_agg_data_status_t
    uint8_t len;
} ble_item_t;

static RingbufHandle_t s_ring;
static TaskHandle_t s_task;
static volatile scan_mode_t s_mode;  // from Configure
static volatile bool s_running;
static volatile ble_sniffer_stats_t s_stats;

// Task-owned.
static ble_agg_t s_agg;
static scan_mode_t s_scan;  // what this task last started
static int s_last_rc;       // last scan start error logged
static uint64_t s_retry_ms; // no scan start before this time
static int s_mbufs_min = -1;

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static uint8_t addr_type_of(uint8_t hci_type)
{
    switch (hci_type) {
    case BLE_ADDR_PUBLIC:
    case BLE_ADDR_PUBLIC_ID:  // resolved by the controller
        return ns_board_v1_BleAddressType_BLE_ADDRESS_TYPE_PUBLIC;
    case BLE_ADDR_RANDOM:
    case BLE_ADDR_RANDOM_ID:
        return ns_board_v1_BleAddressType_BLE_ADDRESS_TYPE_RANDOM;
    default:  // 0xFF: extended advertising without an address
        return ns_board_v1_BleAddressType_BLE_ADDRESS_TYPE_ANONYMOUS;
    }
}

static uint8_t adv_type_of(const struct ble_gap_ext_disc_desc *d)
{
    if (!(d->props & BLE_HCI_ADV_LEGACY_MASK)) {
        return ns_board_v1_BleAdvType_BLE_ADV_TYPE_EXTENDED;
    }
    switch (d->legacy_event_type) {
    case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
        return ns_board_v1_BleAdvType_BLE_ADV_TYPE_ADV_IND;
    case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:
        return ns_board_v1_BleAdvType_BLE_ADV_TYPE_ADV_DIRECT_IND;
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
        return ns_board_v1_BleAdvType_BLE_ADV_TYPE_ADV_SCAN_IND;
    case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:
        return ns_board_v1_BleAdvType_BLE_ADV_TYPE_ADV_NONCONN_IND;
    default:
        return ns_board_v1_BleAdvType_BLE_ADV_TYPE_UNSPECIFIED;
    }
}

// NimBLE host task: copy the report and get out.
static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        xTaskNotifyGive(s_task);  // the task restarts the scan
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_EXT_DISC) {
        return 0;
    }
    const struct ble_gap_ext_disc_desc *d = &event->ext_disc;
    ble_item_t *item = NULL;
    if (xRingbufferSendAcquire(s_ring, (void **)&item, sizeof(*item) + d->length_data, 0) != pdTRUE) {
        s_stats.dropped_reports++;
        return 0;
    }
    item->time_ms = (uint32_t)now_ms();
    // NimBLE keeps addresses least significant byte first.
    for (int i = 0; i < 6; i++) {
        item->addr[i] = d->addr.val[5 - i];
    }
    item->addr_type = addr_type_of(d->addr.type);
    item->adv_type = adv_type_of(d);
    item->rssi = d->rssi;
    item->tx_power = d->tx_power;
    item->data_status = d->data_status;
    item->len = d->length_data;
    memcpy(item + 1, d->data, d->length_data);
    xRingbufferSendComplete(s_ring, item);
    s_stats.reports++;
    return 0;
}

static void stop_scan(void)
{
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    s_scan = SCAN_OFF;
}

static void start_scan(scan_mode_t mode)
{
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        return;
    }
    struct ble_gap_ext_disc_params params = {
        .itvl = BLE_SNIFFER_INTERLEAVE_ITVL,
        .window = mode == SCAN_CONTINUOUS ? BLE_SNIFFER_INTERLEAVE_ITVL : BLE_SNIFFER_INTERLEAVE_WINDOW,
        .passive = 1,  // never send scan requests
    };
    // No duplicate filtering (counts and RSSI per window), 1M PHY only.
    int rc = ble_gap_ext_disc(own_addr_type, 0, 0, 0, BLE_HCI_SCAN_FILT_NO_WL, 0, &params, NULL, on_gap_event,
                              NULL);
    if (rc == 0) {
        s_scan = mode;
        s_stats.scan_starts++;
        s_last_rc = 0;
        ESP_LOGI(TAG, "%s scan started", mode == SCAN_CONTINUOUS ? "continuous" : "interleaved");
    } else if (rc != BLE_HS_EALREADY) {
        if (rc != s_last_rc) {
            ESP_LOGW(TAG, "scan start: rc=%d (retrying every %d ms)", rc, RETRY_MS);
            s_last_rc = rc;
        }
        s_retry_ms = now_ms() + RETRY_MS;
    }
}

// Brings the scan in line with what is wanted. Also restarts a scan that was
// cancelled behind our back (bond changes) or lost in a host reset.
static void ensure_scan(scan_mode_t want)
{
    if (!ble_hs_synced()) {
        s_scan = SCAN_OFF;
        return;
    }
    bool active = ble_gap_disc_active();
    if (want == SCAN_OFF) {
        if (s_scan != SCAN_OFF || active) {
            stop_scan();
        }
        return;
    }
    if (active && s_scan != want) {
        stop_scan();
        active = false;
    }
    if (!active && now_ms() >= s_retry_ms) {
        start_scan(want);
    }
}

static ble_agg_record_fn s_record;
static void *s_record_ctx;

static void on_record(const ns_board_v1_Record *rec, void *ctx)
{
    (void)ctx;
    s_stats.observations++;
    s_record(rec, s_record_ctx);
}

static void flush(void)
{
    ble_agg_flush(&s_agg);
    s_stats.windows++;
}

static void add_item(const ble_item_t *item)
{
    uint64_t now = now_ms();
    ble_agg_report_t r = {
        .addr = item->addr,
        .addr_type = item->addr_type,
        .adv_type = item->adv_type,
        .rssi = item->rssi,
        .tx_power = item->tx_power,
        .data_status = item->data_status,
        .data = (const uint8_t *)(item + 1),
        .len = item->len,
        .time_ms = now - (uint32_t)((uint32_t)now - item->time_ms),
    };
    if (!ble_agg_add(&s_agg, &r)) {
        flush();  // table full: end this window early
        ble_agg_add(&s_agg, &r);
    }
}

static void drain(void)
{
    size_t size;
    ble_item_t *item;
    while ((item = xRingbufferReceive(s_ring, &size, 0)) != NULL) {
        add_item(item);
        vRingbufferReturnItem(s_ring, item);
    }
}

static void sniffer_task(void *arg)
{
    (void)arg;
    uint64_t window_end = 0;
    uint64_t summary_at = 0;
    bool paused = false;  // short of NimBLE buffers until window_end
    ble_sniffer_stats_t last = {0};
    for (;;) {
        int mbufs = os_msys_num_free();
        if (s_mbufs_min < 0 || mbufs < s_mbufs_min) {
            s_mbufs_min = mbufs;
        }
        if (!paused && s_scan != SCAN_OFF && mbufs < MIN_FREE_MBUFS) {
            paused = true;
            s_stats.paused_windows++;
        }
        scan_mode_t want = s_running && !paused ? s_mode : SCAN_OFF;
        ensure_scan(want);
        uint64_t now = now_ms();
        if (!s_running || s_mode == SCAN_OFF) {
            paused = false;
        }
        if (want == SCAN_OFF && !paused) {
            drain();
            if (s_agg.count > 0) {
                flush();
            }
            window_end = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (window_end == 0) {
            window_end = now + BLE_SNIFFER_WINDOW_MS;
            summary_at = now + SUMMARY_MS;
        }
        if (now >= window_end) {
            drain();
            flush();
            window_end = now + BLE_SNIFFER_WINDOW_MS;
            paused = false;
        }
        if (now >= summary_at) {
            ESP_LOGI(TAG, "last %d s: %u reports (%u dropped), %u observations, %u paused windows, min free mbufs %d",
                     SUMMARY_MS / 1000, (unsigned)(s_stats.reports - last.reports),
                     (unsigned)(s_stats.dropped_reports - last.dropped_reports),
                     (unsigned)(s_stats.observations - last.observations),
                     (unsigned)(s_stats.paused_windows - last.paused_windows), s_mbufs_min);
            memcpy(&last, (const void *)&s_stats, sizeof(last));
            summary_at = now + SUMMARY_MS;
            s_mbufs_min = -1;
        }
        uint64_t left = window_end - now;
        TickType_t ticks = pdMS_TO_TICKS(left < POLL_MS ? left : POLL_MS);
        size_t size;
        ble_item_t *item = xRingbufferReceive(s_ring, &size, ticks > 0 ? ticks : 1);
        if (item != NULL) {
            add_item(item);
            vRingbufferReturnItem(s_ring, item);
            drain();
        }
        ulTaskNotifyTake(pdTRUE, 0);  // state changes are picked up above
    }
}

esp_err_t ble_sniffer_init(ble_agg_record_fn record, void *ctx)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_record = record;
    s_record_ctx = ctx;
    uint16_t capacity = TABLE_PSRAM;
    ble_agg_entry_t *table = heap_caps_malloc(capacity * sizeof(*table), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (table == NULL) {
        capacity = TABLE_INTERNAL;
        table = heap_caps_malloc(capacity * sizeof(*table), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    s_ring = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (table == NULL || s_ring == NULL) {
        goto fail;
    }
    ble_agg_init(&s_agg, table, capacity, on_record, NULL);
    if (xTaskCreate(sniffer_task, "ble_sniffer", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        goto fail;
    }
    ESP_LOGI(TAG, "ready: %u advertisers per window", capacity);
    return ESP_OK;

fail:
    heap_caps_free(table);
    if (s_ring != NULL) {
        vRingbufferDelete(s_ring);
        s_ring = NULL;
    }
    return ESP_ERR_NO_MEM;
}

void ble_sniffer_configure(const ns_board_v1_Configure *cfg)
{
    if (cfg->role == ns_board_v1_Role_ROLE_BLE_SNIFFER) {
        s_mode = SCAN_CONTINUOUS;
    } else {
        s_mode = cfg->ble ? SCAN_INTERLEAVE : SCAN_OFF;
    }
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

esp_err_t ble_sniffer_start(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_running = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t ble_sniffer_stop(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_running = false;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void ble_sniffer_get_stats(ble_sniffer_stats_t *out)
{
    memcpy(out, (const void *)&s_stats, sizeof(*out));
}
