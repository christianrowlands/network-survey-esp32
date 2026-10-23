// Debug sink for bring-up: logs what each dwell produced instead of sending
// records. Logs counts and sizes only, never SSIDs or addresses.

#include "esp_log.h"
#include "esp_timer.h"
#include "ns_proto_fit.h"
#include "pb_encode.h"
#include "sniffer.h"

static const char *TAG = "sniffer_dbg";

#define STATS_PERIOD_US (5 * 1000 * 1000)

static uint32_t s_records[3];       // AP, client, probe records since the last stats line
static uint32_t s_max_size[3];      // Largest encoded record of each kind
static uint32_t s_security[9];      // AP records per SecurityType

static void on_record(const ns_board_v1_Record *rec, void *ctx)
{
    (void)ctx;
    int k = rec->which_kind == ns_board_v1_Record_ap_tag       ? 0
            : rec->which_kind == ns_board_v1_Record_client_tag ? 1
                                                               : 2;
    s_records[k]++;
    // Do what the link does per record (copy, fit to MTU 185, encode) so the
    // stack high-water mark reflects a real sink.
    ns_board_v1_Record copy = *rec;
    uint8_t buf[256];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    if (ns_proto_fit_record(&copy, ns_proto_max_record_size(185))) {
        pb_encode(&os, ns_board_v1_Record_fields, &copy);
    }
    uint32_t size = (uint32_t)ns_proto_record_size(rec);
    if (size > s_max_size[k]) {
        s_max_size[k] = size;
    }
    if (k == 0 && (unsigned)rec->kind.ap.security < 9) {
        s_security[rec->kind.ap.security]++;
    }
}

static void on_dwell(const aggregator_dwell_summary_t *s, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "ch %3u %4u ms: frames %4u, ap %2u, client %2u, probe %2u, full %u",
             (unsigned)s->channel, (unsigned)s->dwell_ms, (unsigned)s->frames, s->aps, s->clients,
             s->probes, s->dropped);
}

static void log_stats(void *arg)
{
    (void)arg;
    sniffer_stats_t st;
    sniffer_get_stats(&st);
    ESP_LOGI(TAG, "rx %u dropped %u truncated %u mismatch %u stale %u agg_full %u hops %u "
             "max_hop %u us max_len %u stack_free %u",
             (unsigned)st.rx_frames, (unsigned)st.dropped_rx_frames, (unsigned)st.truncated_frames,
             (unsigned)st.channel_mismatch, (unsigned)st.stale_frames, (unsigned)st.agg_dropped,
             (unsigned)st.hops, (unsigned)st.max_hop_us, (unsigned)st.max_frame_len,
             (unsigned)st.stack_free_min);
    ESP_LOGI(TAG, "records ap %u client %u probe %u, max bytes %u/%u/%u, security "
             "open %u wep %u wpa %u wpa/wpa2 %u wpa2 %u wpa3 %u wpa2/wpa3 %u owe %u",
             (unsigned)s_records[0], (unsigned)s_records[1], (unsigned)s_records[2],
             (unsigned)s_max_size[0], (unsigned)s_max_size[1], (unsigned)s_max_size[2],
             (unsigned)s_security[1], (unsigned)s_security[2], (unsigned)s_security[3],
             (unsigned)s_security[4], (unsigned)s_security[5], (unsigned)s_security[6],
             (unsigned)s_security[7], (unsigned)s_security[8]);
}

esp_err_t sniffer_debug_autostart(void)
{
    static const aggregator_sink_t sink = {.record = on_record, .dwell_done = on_dwell};
    esp_err_t err = sniffer_init(&sink);
    if (err != ESP_OK) {
        return err;
    }
    ns_board_v1_Configure cfg = ns_board_v1_Configure_init_zero;
    cfg.wifi_ap = true;
    cfg.wifi_client = true;
    cfg.wifi_probes = true;
    cfg.role = ns_board_v1_Role_ROLE_AUTO;
    sniffer_configure(&cfg, NULL);

    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = log_stats, .name = "sniff_dbg"};
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, STATS_PERIOD_US);
    }
    return sniffer_start();
}
