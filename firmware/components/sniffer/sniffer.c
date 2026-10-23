#include "sniffer.h"

#include <string.h>

#include "channel_planner.h"
#include "dot11_parser.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sniffer_internal.h"

static const char *TAG = "sniffer";

#define MGMT_SNAPLEN 1024   // Header and elements; longer frames lose trailing elements
#define DATA_SNAPLEN 32     // Header (24), QoS control (2), HT control (4)
// Raw frames add their own copy limit (the raw snaplen, sniffer_raw.c).
#define FCS_LEN 4
#define RING_BYTES (24 * 1024)
// The sink (link encode and MTU fitting) runs on this task during flushes.
// 6 KB leaves margin; sniffer_get_stats reports the free minimum.
#define TASK_STACK 6144
#define TASK_PRIO 5
#define IDLE_FLUSH_MS 20    // Close the previous dwell after this long without frames
#define GEN_MASK 0xFFFFFFu  // Dwell generations are 24 bits (see s_tag)

#if CONFIG_SOC_WIFI_SUPPORT_5G
#define SUPPORTS_5G true
#else
#define SUPPORTS_5G false
#endif

static RingbufHandle_t s_ring;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;  // Guards the pending configuration

// Written by the task, read by the driver callback as one word so channel
// and generation always match: (generation << 8) | channel, channel 0 = idle.
static volatile uint32_t s_tag;
static volatile uint8_t s_channel;
static volatile bool s_running;
static volatile bool s_parse_data;  // data frames feed the aggregator (wifi_client)
static volatile sniffer_stats_t s_stats;

// Task-owned state.
static aggregator_t s_agg;
static channel_planner_t s_planner;
static dot11_frame_t s_frame;
static bool s_dwell_open;
static uint32_t s_gen;
static uint32_t s_dwell_gen;
static bool s_out[3] = {true, true, false};  // AP, client, probe outputs for the next dwell

static channel_planner_t s_pending;
static bool s_pending_valid;
static bool s_pending_out[3];

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    uint32_t tag = s_tag;
    uint8_t channel = (uint8_t)tag;
    if (channel == 0 || pkt->rx_ctrl.rx_state != 0) {
        return;
    }
    uint32_t sig_len = pkt->rx_ctrl.sig_len;  // Includes the FCS
    if (sig_len <= FCS_LEN) {
        return;
    }
    uint32_t len = sig_len - FCS_LEN;
    int64_t now_us = esp_timer_get_time();
    bool parse = type == WIFI_PKT_MGMT || (type == WIFI_PKT_DATA && s_parse_data);
    uint16_t raw = sniffer_raw_take(type, pkt->payload, len, (uint64_t)now_us);
    if (!parse && raw == 0) {
        return;
    }
    uint32_t cap = !parse ? 0 : type == WIFI_PKT_MGMT ? MGMT_SNAPLEN : DATA_SNAPLEN;
    if (raw > cap) {
        cap = raw;
    }
    uint32_t copy = len < cap ? len : cap;
    if (parse && len > s_stats.max_frame_len) {
        s_stats.max_frame_len = len;
    }
    if (parse && type == WIFI_PKT_MGMT && copy < len) {
        s_stats.truncated_frames++;
    }

    rx_item_t *item = NULL;
    if (xRingbufferSendAcquire(s_ring, (void **)&item, sizeof(*item) + copy, 0) != pdTRUE) {
        if (parse) {
            s_stats.dropped_rx_frames++;
        }
        if (raw != 0) {
            sniffer_raw_lost();
        }
        return;
    }
    item->gen = tag >> 8;
    item->time_us = (uint32_t)now_us;
    item->len = (uint16_t)copy;
    item->orig_len = (uint16_t)len;
    item->raw_len = raw;
    item->rssi = (int8_t)pkt->rx_ctrl.rssi;
    item->noise_floor = (int8_t)pkt->rx_ctrl.noise_floor;
    item->channel = channel;
    item->rx_channel = (uint8_t)pkt->rx_ctrl.channel;
    item->flags = (parse ? ITEM_PARSE : 0) | (raw != 0 ? ITEM_RAW : 0) | (copy < len ? ITEM_TRUNCATED : 0);
    if (raw != 0) {
        sniffer_raw_meta(&pkt->rx_ctrl, item);
    }
    memcpy(item + 1, pkt->payload, copy);
    xRingbufferSendComplete(s_ring, item);
    if (parse) {
        s_stats.rx_frames++;
    }
}

// Data frames only matter for client observations (and raw data frames);
// leaving them out keeps the ring free for management frames.
static void set_filter(void)
{
    s_parse_data = s_out[1];
    radio_set_filter(WIFI_PROMIS_FILTER_MASK_MGMT | (s_out[1] ? WIFI_PROMIS_FILTER_MASK_DATA : 0) |
                     sniffer_raw_filter());
}

static void publish(uint8_t channel)
{
    s_channel = channel;
    s_tag = (s_gen << 8) | channel;
}

static void end_dwell(void)
{
    if (s_dwell_open) {
        s_stats.agg_dropped += s_agg.dropped;
        uint32_t aps = aggregator_flush(&s_agg, now_ms());
        channel_planner_report(&s_planner, (uint8_t)s_agg.channel, aps);
        s_dwell_open = false;
    }
}

static void begin_dwell(uint32_t gen, uint8_t channel)
{
    end_dwell();
    aggregator_set_outputs(&s_agg, s_out[0], s_out[1], s_out[2]);
    aggregator_begin(&s_agg, channel, now_ms());
    s_dwell_gen = gen;
    s_dwell_open = true;
}

static void process(const rx_item_t *item)
{
    // The driver may still deliver frames from the previous channel just after a hop.
    if (item->rx_channel != 0 && item->rx_channel != item->channel) {
        s_stats.channel_mismatch++;
        if (item->flags & ITEM_RAW) {
            sniffer_raw_task_lost();
        }
        return;
    }
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (item->flags & ITEM_RAW) {
        sniffer_raw_emit(item, now_us);
    }
    if (!(item->flags & ITEM_PARSE)) {
        return;
    }
    if (!s_dwell_open || item->gen != s_dwell_gen) {
        if (s_dwell_open && ((item->gen - s_dwell_gen) & GEN_MASK) >= (GEN_MASK + 1) / 2) {
            s_stats.stale_frames++;
            return;
        }
        begin_dwell(item->gen, item->channel);
    }
    if (!dot11_parse((const uint8_t *)(item + 1), item->len, &s_frame)) {
        return;
    }
    s_frame.truncated = s_frame.truncated || (item->flags & ITEM_TRUNCATED);
    // Rebuild the 64-bit time from the 32-bit stamp (frames are milliseconds old).
    uint64_t time_us = now_us - (uint32_t)((uint32_t)now_us - item->time_us);
    aggregator_rx_t rx = {
        .channel = item->channel,
        .rssi = item->rssi,
        .has_noise_floor = item->noise_floor != 0,
        .noise_floor = item->noise_floor,
        .time_ms = time_us / 1000,
    };
    aggregator_add(&s_agg, &s_frame, &rx);
}

// At a dwell boundary: take a pending Configure. The channel schedule and
// filter change now; the outputs apply from the next dwell the aggregator opens.
static void apply_pending(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending_valid) {
        s_planner = s_pending;
        memcpy(s_out, s_pending_out, sizeof(s_out));
        s_pending_valid = false;
    }
    xSemaphoreGive(s_lock);
    sniffer_raw_update();
    set_filter();
}

static void go_idle(void)
{
    if (s_channel != 0) {
        publish(0);
        esp_wifi_set_promiscuous(false);
    }
    size_t size;
    rx_item_t *item;
    while ((item = xRingbufferReceive(s_ring, &size, 0)) != NULL) {
        process(item);
        vRingbufferReturnItem(s_ring, item);
    }
    end_dwell();
}

static void sniffer_task(void *arg)
{
    (void)arg;
    uint64_t dwell_end = 0;
    for (;;) {
        uint64_t now = now_ms();
        bool boundary = s_channel == 0 || now >= dwell_end;
        if (boundary) {
            apply_pending();
        }
        if (!s_running || s_planner.count == 0) {
            // Idle, or ROLE_BLE_SNIFFER: promiscuous mode off. Wi-Fi itself
            // stays started: on the ESP32-C5 a reset taken while Wi-Fi was
            // stopped and BLE was scanning left both radios receiving poorly
            // after the next boot. Idle Wi-Fi costs BLE about 15% of reports.
            go_idle();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (boundary) {
            if (s_channel == 0) {
                esp_wifi_set_promiscuous(true);
            }
            cp_hop_t hop;
            channel_planner_next(&s_planner, &hop);
            if (hop.channel != s_channel) {
                uint32_t took = radio_tune(hop.channel);
                if (took > s_stats.max_hop_us) {
                    s_stats.max_hop_us = took;
                }
                s_stats.hops++;
            }
            s_gen = (s_gen + 1) & GEN_MASK;
            publish(hop.channel);
            dwell_end = now_ms() + hop.dwell_ms;
            continue;
        }

        uint64_t left = dwell_end - now;
        TickType_t ticks = pdMS_TO_TICKS(left < IDLE_FLUSH_MS ? left : IDLE_FLUSH_MS);
        size_t size;
        rx_item_t *item = xRingbufferReceive(s_ring, &size, ticks > 0 ? ticks : 1);
        if (item != NULL) {
            process(item);
            vRingbufferReturnItem(s_ring, item);
        } else if (!s_dwell_open || s_dwell_gen != s_gen) {
            // Queue drained since the hop: close the old dwell, open the new one.
            begin_dwell(s_gen, s_channel);
        }
    }
}

esp_err_t sniffer_init(const aggregator_sink_t *sink)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    aggregator_init(&s_agg, sink);
    channel_planner_init(&s_planner, SUPPORTS_5G);
    s_lock = xSemaphoreCreateMutex();
    s_ring = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (s_lock == NULL || s_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(radio_init(promisc_cb), TAG, "Wi-Fi init");
    set_filter();
    if (xTaskCreate(sniffer_task, "sniffer", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready: %u channels, 5 GHz %s", s_planner.count, SUPPORTS_5G ? "yes" : "no");
    return ESP_OK;
}

ns_board_v1_ResultCode sniffer_configure(const ns_board_v1_Configure *cfg,
                                         ns_board_v1_ConfigureResult *result)
{
    if (s_task == NULL) {
        return ns_board_v1_ResultCode_RESULT_CODE_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = channel_planner_configure(&s_pending, cfg, SUPPORTS_5G);
    if (ok) {
        sniffer_raw_configure(cfg);
        s_pending_valid = true;
        s_pending_out[0] = cfg->wifi_ap;
        s_pending_out[1] = cfg->wifi_client;
        s_pending_out[2] = cfg->wifi_probes;
        if (result != NULL) {
            memset(result, 0, sizeof(*result));
            result->channels_count = s_pending.count;
            for (uint8_t i = 0; i < s_pending.count; i++) {
                result->channels[i] = s_pending.channels[i];
            }
            result->role = cfg->role;
        }
    }
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return ok ? ns_board_v1_ResultCode_RESULT_CODE_OK : ns_board_v1_ResultCode_RESULT_CODE_INVALID_ARGUMENT;
}

esp_err_t sniffer_start(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_running = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t sniffer_stop(void)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_running = false;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

uint32_t sniffer_current_channel(void) { return s_channel; }

void sniffer_get_stats(sniffer_stats_t *out)
{
    memcpy(out, (const void *)&s_stats, sizeof(*out));
    out->stack_free_min = s_task != NULL ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
}
