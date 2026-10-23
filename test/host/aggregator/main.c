// Host tests for the aggregator: keying, per-dwell summaries, flush output,
// table limits, and that every record encodes and fits the MTU budget.

#include <stdio.h>
#include <string.h>

#include "../common/frame_builder.h"
#include "aggregator.h"
#include "ns_proto_fit.h"

static int s_failures;
static int s_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        s_checks++;                                                          \
        if (!(cond)) {                                                       \
            s_failures++;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b) CHECK((long long)(a) == (long long)(b))

#define RSN(t) (0x000FAC00u | (t))
#define MAX_OUT 256

// Collecting sink.
static ns_board_v1_Record s_out[MAX_OUT];
static int s_out_count;
static aggregator_dwell_summary_t s_summary;
static int s_summaries;

static void on_record(const ns_board_v1_Record *rec, void *ctx)
{
    (void)ctx;
    if (s_out_count < MAX_OUT) {
        s_out[s_out_count++] = *rec;
    }
}

static void on_dwell(const aggregator_dwell_summary_t *s, void *ctx)
{
    (void)ctx;
    s_summary = *s;
    s_summaries++;
}

static aggregator_t s_agg;

static void reset(void)
{
    static const aggregator_sink_t sink = {.record = on_record, .dwell_done = on_dwell};
    aggregator_init(&s_agg, &sink);
    s_out_count = 0;
    s_summaries = 0;
}

static aggregator_rx_t rx(uint32_t channel, int32_t rssi, uint64_t t)
{
    aggregator_rx_t r = {.channel = channel, .rssi = rssi, .has_noise_floor = true,
                         .noise_floor = -95, .time_ms = t};
    return r;
}

static void feed(const fb_t *f, aggregator_rx_t r)
{
    dot11_frame_t p;
    dot11_parse(f->buf, f->len, &p);
    aggregator_add(&s_agg, &p, &r);
}

// As the sniffer does for a frame longer than its copy limit: parse the
// prefix and mark the frame truncated, even when the cut falls on an element
// boundary.
static void feed_cut(const fb_t *f, size_t n, aggregator_rx_t r)
{
    dot11_frame_t p;
    dot11_parse(f->buf, n, &p);
    p.truncated = p.truncated || n < f->len;
    aggregator_add(&s_agg, &p, &r);
}

static void beacon(fb_t *f, uint8_t subtype, const uint8_t *bssid, const char *ssid, uint8_t ds)
{
    static const uint8_t rates[] = {0x82, 0x84, 0x8B, 0x96};
    const uint32_t pw[] = {RSN(4)};
    const uint32_t akm[] = {RSN(2)};
    fb_beacon(f, subtype, bssid, 0x0011);
    if (ssid != NULL) {
        fb_ssid(f, ssid);
    }
    fb_ie(f, 1, rates, sizeof(rates));
    if (ds != 0) {
        fb_ie(f, 3, &ds, 1);
    }
    fb_rsn(f, RSN(4), pw, 1, akm, 1);
}

static const ns_board_v1_Record *find(pb_size_t kind, int nth)
{
    for (int i = 0; i < s_out_count; i++) {
        if (s_out[i].which_kind == kind && nth-- == 0) {
            return &s_out[i];
        }
    }
    return NULL;
}

static void test_ap_keying(void)
{
    reset();
    aggregator_begin(&s_agg, 6, 1000);
    fb_t f;
    beacon(&f, 8, FB_AP1, "ExampleNet", 6);
    feed(&f, rx(6, -70, 1010));
    feed(&f, rx(6, -60, 1110));
    feed(&f, rx(6, -65, 1210));
    beacon(&f, 5, FB_AP1, "ExampleNet", 6);
    feed(&f, rx(6, -62, 1220));
    beacon(&f, 8, FB_AP2, "Cafe-Guest", 0);  // no DS element: receive channel
    feed(&f, rx(6, -80, 1230));

    uint32_t aps = aggregator_flush(&s_agg, 1250);
    CHECK_EQ(aps, 2);
    CHECK_EQ(s_out_count, 2);
    CHECK_EQ(s_summaries, 1);
    CHECK_EQ(s_summary.channel, 6);
    CHECK_EQ(s_summary.dwell_ms, 250);
    CHECK_EQ(s_summary.frames, 5);
    CHECK_EQ(s_summary.aps, 2);
    CHECK_EQ(s_summary.clients, 0);

    const ns_board_v1_Record *r = find(ns_board_v1_Record_ap_tag, 0);
    CHECK(r != NULL);
    const ns_board_v1_ApObservation *ap = &r->kind.ap;
    CHECK(memcmp(ap->bssid, FB_AP1, 6) == 0);
    CHECK_EQ(ap->beacon_count, 3);
    CHECK_EQ(ap->probe_response_count, 1);
    CHECK_EQ(ap->rssi_max, -60);
    CHECK_EQ(ap->rssi_last, -62);
    CHECK_EQ(ap->board_time_ms, 1220);
    CHECK_EQ(ap->channel, 6);
    CHECK_EQ(ap->frequency_mhz, 2437);
    CHECK_EQ(ap->band, ns_board_v1_Band_BAND_GHZ_2_4);
    CHECK(ap->has_noise_floor_dbm && ap->noise_floor_dbm == -95);
    CHECK_EQ(ap->ssid.size, 10);
    CHECK(memcmp(ap->ssid.bytes, "ExampleNet", 10) == 0);
    CHECK(!ap->ssid_hidden);
    CHECK(ap->has_beacon_interval_tu && ap->beacon_interval_tu == 100);
    CHECK_EQ(ap->capability_info, 0x0011);
    CHECK_EQ(ap->security, ns_board_v1_SecurityType_SECURITY_TYPE_WPA2);
    CHECK_EQ(ap->group_cipher, RSN(4));
    CHECK_EQ(ap->pairwise_ciphers_count, 1);
    CHECK_EQ(ap->akm_suites_count, 1);
    CHECK_EQ(ap->akm_suites[0], RSN(2));
    CHECK_EQ(ap->supported_rates.size, 4);
    CHECK(!ap->has_station_count);

    r = find(ns_board_v1_Record_ap_tag, 1);
    CHECK(r != NULL && memcmp(r->kind.ap.bssid, FB_AP2, 6) == 0);
    CHECK_EQ(r->kind.ap.beacon_count, 1);
    CHECK_EQ(r->kind.ap.rssi_max, -80);

    // Flushed: the next dwell starts empty.
    s_out_count = 0;
    aps = aggregator_flush(&s_agg, 1300);
    CHECK_EQ(aps, 0);
    CHECK_EQ(s_out_count, 0);
    CHECK_EQ(s_summary.frames, 0);
}

static void test_ap_channel(void)
{
    reset();
    fb_t f;
    // Adjacent-channel reception: the AP says channel 5, the radio is on 6.
    aggregator_begin(&s_agg, 6, 0);
    beacon(&f, 8, FB_AP1, "ExampleNet", 5);
    feed(&f, rx(6, -70, 1));
    // An element channel from the other band is ignored.
    beacon(&f, 8, FB_AP2, "ExampleNet", 36);
    feed(&f, rx(6, -70, 2));
    aggregator_flush(&s_agg, 10);
    CHECK_EQ(find(ns_board_v1_Record_ap_tag, 0)->kind.ap.channel, 5);
    CHECK_EQ(find(ns_board_v1_Record_ap_tag, 0)->kind.ap.frequency_mhz, 2432);
    CHECK_EQ(find(ns_board_v1_Record_ap_tag, 1)->kind.ap.channel, 6);

    // 5 GHz: HT Operation primary channel when there is no DS element.
    reset();
    aggregator_begin(&s_agg, 40, 0);
    static const uint8_t ht_op[22] = {36, 0x05};
    beacon(&f, 8, FB_AP1, "ExampleNet", 0);
    fb_ie(&f, 61, ht_op, sizeof(ht_op));
    feed(&f, rx(40, -70, 1));
    aggregator_flush(&s_agg, 10);
    const ns_board_v1_ApObservation *ap = &find(ns_board_v1_Record_ap_tag, 0)->kind.ap;
    CHECK_EQ(ap->channel, 36);
    CHECK_EQ(ap->band, ns_board_v1_Band_BAND_GHZ_5);
    CHECK_EQ(ap->frequency_mhz, 5180);
    CHECK_EQ(ap->channel_width, ns_board_v1_ChannelWidth_CHANNEL_WIDTH_MHZ_40);
}

static void test_hidden_ssid(void)
{
    reset();
    aggregator_begin(&s_agg, 1, 0);
    fb_t f;
    beacon(&f, 8, FB_AP1, "", 1);  // hidden beacon
    feed(&f, rx(1, -70, 1));
    beacon(&f, 5, FB_AP1, "ExampleNet", 1);  // probe response reveals it
    feed(&f, rx(1, -70, 2));
    beacon(&f, 8, FB_AP1, "", 1);
    feed(&f, rx(1, -70, 3));
    aggregator_flush(&s_agg, 10);
    const ns_board_v1_ApObservation *ap = &find(ns_board_v1_Record_ap_tag, 0)->kind.ap;
    CHECK(ap->ssid_hidden);
    CHECK_EQ(ap->ssid.size, 10);
    CHECK_EQ(ap->beacon_count, 2);
    CHECK_EQ(ap->probe_response_count, 1);

    // Hidden only: empty SSID, flag set.
    reset();
    aggregator_begin(&s_agg, 1, 0);
    beacon(&f, 8, FB_AP2, "", 1);
    feed(&f, rx(1, -70, 1));
    aggregator_flush(&s_agg, 10);
    ap = &find(ns_board_v1_Record_ap_tag, 0)->kind.ap;
    CHECK(ap->ssid_hidden);
    CHECK_EQ(ap->ssid.size, 0);
}

static void test_clients(void)
{
    reset();
    aggregator_begin(&s_agg, 11, 0);
    fb_t f;
    // QoS data to the AP, then a null frame, from STA1.
    fb_header(&f, FB_FC(2, 8), 0x01, FB_AP1, FB_STA1, FB_BCAST);
    feed(&f, rx(11, -55, 10));
    fb_header(&f, FB_FC(2, 4), 0x11, FB_AP1, FB_STA1, FB_AP1);
    feed(&f, rx(11, -75, 20));
    // Data from the AP to STA2 does not describe STA2.
    fb_header(&f, FB_FC(2, 8), 0x02, FB_STA2, FB_AP1, FB_AP1);
    feed(&f, rx(11, -40, 30));
    // STA2 probes, then authenticates and associates to AP2.
    fb_header(&f, FB_FC(0, 4), 0, FB_BCAST, FB_STA2, FB_BCAST);
    fb_ssid(&f, "ExampleNet");
    feed(&f, rx(11, -66, 40));
    fb_header(&f, FB_FC(0, 11), 0, FB_AP2, FB_STA2, FB_AP2);
    fb_le16(&f, 0);
    fb_le16(&f, 1);
    fb_le16(&f, 0);
    feed(&f, rx(11, -64, 50));
    // The AP's authentication reply (address 2 = BSSID) is not a client frame.
    fb_header(&f, FB_FC(0, 11), 0, FB_STA2, FB_AP2, FB_AP2);
    fb_le16(&f, 0);
    fb_le16(&f, 2);
    fb_le16(&f, 0);
    feed(&f, rx(11, -30, 55));
    fb_header(&f, FB_FC(0, 0), 0, FB_AP2, FB_STA2, FB_AP2);
    fb_le16(&f, 0x0431);
    fb_le16(&f, 10);
    fb_ssid(&f, "ExampleNet");
    feed(&f, rx(11, -63, 60));
    fb_header(&f, FB_FC(0, 2), 0, FB_AP2, FB_STA2, FB_AP2);
    fb_le16(&f, 0x0431);
    fb_le16(&f, 10);
    fb_bytes(&f, FB_AP1, 6);
    feed(&f, rx(11, -63, 65));
    fb_header(&f, FB_FC(0, 13), 0, FB_AP2, FB_STA2, FB_AP2);
    fb_u8(&f, 4);
    feed(&f, rx(11, -62, 70));
    // A later probe does not clear the known BSS.
    fb_header(&f, FB_FC(0, 4), 0, FB_BCAST, FB_STA2, FB_BCAST);
    fb_ie(&f, 0, "", 0);
    feed(&f, rx(11, -61, 80));

    aggregator_flush(&s_agg, 100);
    CHECK_EQ(s_summary.clients, 2);
    CHECK_EQ(s_summary.probes, 0);  // probe output is off by default
    CHECK_EQ(s_summary.frames, 10);

    const ns_board_v1_ClientObservation *c = &find(ns_board_v1_Record_client_tag, 0)->kind.client;
    CHECK(memcmp(c->client_mac, FB_STA1, 6) == 0);
    CHECK_EQ(c->associated_bssid.size, 6);
    CHECK(memcmp(c->associated_bssid.bytes, FB_AP1, 6) == 0);
    CHECK_EQ(c->frame_count, 2);
    CHECK_EQ(c->rssi_max, -55);
    CHECK_EQ(c->rssi_last, -75);
    CHECK_EQ(c->source_flags, ns_board_v1_ClientSource_CLIENT_SOURCE_DATA |
                                  ns_board_v1_ClientSource_CLIENT_SOURCE_NULL_DATA);
    CHECK(c->locally_administered);
    CHECK_EQ(c->channel, 11);
    CHECK_EQ(c->band, ns_board_v1_Band_BAND_GHZ_2_4);
    CHECK_EQ(c->board_time_ms, 20);

    c = &find(ns_board_v1_Record_client_tag, 1)->kind.client;
    CHECK(memcmp(c->client_mac, FB_STA2, 6) == 0);
    CHECK(!c->locally_administered);
    CHECK_EQ(c->frame_count, 6);
    CHECK_EQ(c->rssi_max, -61);
    CHECK_EQ(c->associated_bssid.size, 6);
    CHECK(memcmp(c->associated_bssid.bytes, FB_AP2, 6) == 0);
    CHECK_EQ(c->source_flags, ns_board_v1_ClientSource_CLIENT_SOURCE_PROBE_REQ |
                                  ns_board_v1_ClientSource_CLIENT_SOURCE_AUTH |
                                  ns_board_v1_ClientSource_CLIENT_SOURCE_ASSOC_REQ |
                                  ns_board_v1_ClientSource_CLIENT_SOURCE_REASSOC_REQ |
                                  ns_board_v1_ClientSource_CLIENT_SOURCE_ACTION);

    // Probe-only client: no BSS.
    reset();
    aggregator_begin(&s_agg, 1, 0);
    fb_header(&f, FB_FC(0, 4), 0, FB_BCAST, FB_STA1, FB_BCAST);
    fb_ie(&f, 0, "", 0);
    feed(&f, rx(1, -61, 80));
    aggregator_flush(&s_agg, 100);
    CHECK_EQ(find(ns_board_v1_Record_client_tag, 0)->kind.client.associated_bssid.size, 0);
}

static void test_probes(void)
{
    reset();
    aggregator_set_outputs(&s_agg, false, false, true);
    aggregator_begin(&s_agg, 1, 0);
    fb_t f;
    static const uint8_t ht_cap[26] = {0};
    // STA1: wildcard twice, ExampleNet once (directed); STA2: ExampleNet.
    fb_header(&f, FB_FC(0, 4), 0, FB_BCAST, FB_STA1, FB_BCAST);
    fb_ie(&f, 0, "", 0);
    fb_ie(&f, 45, ht_cap, sizeof(ht_cap));
    feed(&f, rx(1, -70, 1));
    feed(&f, rx(1, -50, 2));
    fb_header(&f, FB_FC(0, 4), 0, FB_AP1, FB_STA1, FB_AP1);
    fb_ssid(&f, "ExampleNet");
    feed(&f, rx(1, -60, 3));
    fb_header(&f, FB_FC(0, 4), 0, FB_BCAST, FB_STA2, FB_BCAST);
    fb_ssid(&f, "ExampleNet");
    feed(&f, rx(1, -60, 4));
    // Beacons and data are ignored with AP and client output off.
    beacon(&f, 8, FB_AP1, "ExampleNet", 1);
    feed(&f, rx(1, -60, 5));

    aggregator_flush(&s_agg, 10);
    CHECK_EQ(s_out_count, 3);
    CHECK_EQ(s_summary.aps, 0);
    CHECK_EQ(s_summary.clients, 0);
    CHECK_EQ(s_summary.probes, 3);

    const ns_board_v1_ProbeObservation *p = &find(ns_board_v1_Record_probe_tag, 0)->kind.probe;
    CHECK(memcmp(p->client_mac, FB_STA1, 6) == 0);
    CHECK_EQ(p->ssid.size, 0);
    CHECK_EQ(p->count, 2);
    CHECK_EQ(p->rssi_max, -50);
    CHECK_EQ(p->rssi_last, -50);
    CHECK(p->locally_administered);
    CHECK_EQ(p->phy_flags, ns_board_v1_PhyFlag_PHY_FLAG_HT);
    CHECK_EQ(p->destination_address.size, 6);
    CHECK(memcmp(p->destination_address.bytes, FB_BCAST, 6) == 0);
    CHECK_EQ(p->channel, 1);
    CHECK_EQ(p->frequency_mhz, 2412);

    p = &find(ns_board_v1_Record_probe_tag, 1)->kind.probe;
    CHECK(memcmp(p->client_mac, FB_STA1, 6) == 0);
    CHECK_EQ(p->ssid.size, 10);
    CHECK_EQ(p->count, 1);
    CHECK(memcmp(p->bssid.bytes, FB_AP1, 6) == 0);
    CHECK_EQ(p->phy_flags, 0);

    p = &find(ns_board_v1_Record_probe_tag, 2)->kind.probe;
    CHECK(memcmp(p->client_mac, FB_STA2, 6) == 0);
    CHECK(!p->locally_administered);
}

static void test_ignored_frames(void)
{
    reset();
    aggregator_set_outputs(&s_agg, true, true, true);
    aggregator_begin(&s_agg, 1, 0);
    fb_t f;
    // Beacon with a group BSSID, probe from a group address, WDS data.
    beacon(&f, 8, FB_BCAST, "ExampleNet", 1);
    feed(&f, rx(1, -60, 1));
    fb_header(&f, FB_FC(0, 4), 0, FB_BCAST, FB_BCAST, FB_BCAST);
    feed(&f, rx(1, -60, 2));
    fb_header(&f, FB_FC(2, 8), 0x03, FB_AP1, FB_AP2, FB_STA1);
    fb_bytes(&f, FB_STA2, 6);
    feed(&f, rx(1, -60, 3));
    // Action frame from the AP (address 2 = address 3).
    fb_header(&f, FB_FC(0, 13), 0, FB_STA1, FB_AP1, FB_AP1);
    feed(&f, rx(1, -60, 4));
    aggregator_flush(&s_agg, 10);
    CHECK_EQ(s_out_count, 0);
    CHECK_EQ(s_summary.frames, 4);
}

static void test_table_limits(void)
{
    reset();
    aggregator_set_outputs(&s_agg, true, true, true);
    aggregator_begin(&s_agg, 1, 0);
    fb_t f;
    const int extra = 5;
    for (int i = 0; i < AGG_MAX_APS + extra; i++) {
        uint8_t bssid[6] = {0x02, 0x00, 0x5E, 0x30, (uint8_t)(i >> 8), (uint8_t)i};
        beacon(&f, 8, bssid, "ExampleNet", 1);
        feed(&f, rx(1, -60, (uint64_t)i));
        if (i < AGG_MAX_APS) {
            feed(&f, rx(1, -60, (uint64_t)i));  // known keys still update
        }
    }
    for (int i = 0; i < AGG_MAX_CLIENTS + extra; i++) {
        uint8_t mac[6] = {0x0A, 0x00, 0x5E, 0x40, (uint8_t)(i >> 8), (uint8_t)i};
        fb_header(&f, FB_FC(2, 4), 0x01, FB_AP1, mac, FB_AP1);
        feed(&f, rx(1, -60, 1));
    }
    // Probes from the first client, already in the full client table.
    const uint8_t prober[6] = {0x0A, 0x00, 0x5E, 0x40, 0x00, 0x00};
    for (int i = 0; i < AGG_MAX_PROBES + extra; i++) {
        char ssid[24];
        snprintf(ssid, sizeof(ssid), "ExampleNet-%d", i);
        fb_header(&f, FB_FC(0, 4), 0, FB_BCAST, prober, FB_BCAST);
        fb_ssid(&f, ssid);
        feed(&f, rx(1, -60, 1));
    }
    s_out_count = 0;
    aggregator_flush(&s_agg, 10);
    CHECK_EQ(s_summary.aps, AGG_MAX_APS);
    CHECK_EQ(s_summary.clients, AGG_MAX_CLIENTS);
    CHECK_EQ(s_summary.probes, AGG_MAX_PROBES);
    CHECK_EQ(s_summary.dropped, 3 * extra);
    CHECK_EQ(find(ns_board_v1_Record_ap_tag, 0)->kind.ap.beacon_count, 2);

    // Every record encodes within the nanopb limits and fits at MTU 185.
    size_t max_size = ns_proto_max_record_size(185);
    int fit = 0;
    for (int i = 0; i < s_out_count; i++) {
        ns_board_v1_Record r = s_out[i];
        CHECK(ns_proto_record_size(&r) > 0);
        fit += ns_proto_fit_record(&r, max_size);
    }
    CHECK_EQ(fit, s_out_count);
    CHECK_EQ(s_out_count, AGG_MAX_APS + AGG_MAX_CLIENTS + AGG_MAX_PROBES);
}

static void test_dense_ap_fits(void)
{
    // An AP with the longest lists the board keeps still fits after trimming.
    reset();
    aggregator_begin(&s_agg, 36, 0);
    fb_t f;
    uint32_t many[8];
    for (int i = 0; i < 8; i++) {
        many[i] = RSN(i + 1);
    }
    static const uint8_t rates[16] = {0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C,
                                      0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};
    static const uint8_t ext[32] = {0x30};
    static const uint8_t bss_load[5] = {200, 0, 255, 0, 0};
    fb_beacon(&f, 8, FB_AP1, 0x0011);
    fb_ssid(&f, "ExampleNet-With-A-32-Byte-SSID!!");
    fb_ie(&f, 1, rates, sizeof(rates));
    fb_ie(&f, 50, ext, sizeof(ext));
    fb_ie(&f, 11, bss_load, sizeof(bss_load));
    fb_rsn(&f, RSN(4), many, 8, many, 8);
    feed(&f, rx(36, -60, 1));
    aggregator_flush(&s_agg, 10);
    ns_board_v1_Record r = *find(ns_board_v1_Record_ap_tag, 0);
    CHECK_EQ(r.kind.ap.ssid.size, 32);
    CHECK_EQ(r.kind.ap.station_count, 200);
    CHECK(ns_proto_record_size(&r) > ns_proto_max_record_size(185));
    CHECK(ns_proto_fit_record(&r, ns_proto_max_record_size(185)));
    CHECK(r.kind.ap.detail_trimmed);
}

static void test_truncated_frames(void)
{
    fb_t full;
    fb_full_beacon(&full);

    // Element boundaries of the full beacon (after the 36-byte header and fixed fields).
    size_t cuts[32];
    int ncut = 0;
    for (size_t i = 36; i < full.len && ncut < 32; i += 2u + full.buf[i + 1]) {
        cuts[ncut++] = i;
    }
    CHECK(ncut > 10);

    // A complete beacon, then cut copies: nothing it reported is lowered.
    for (int c = 0; c < ncut; c++) {
        reset();
        aggregator_begin(&s_agg, 6, 0);
        feed(&full, rx(6, -60, 1));
        feed_cut(&full, cuts[c], rx(6, -50, 2));
        aggregator_flush(&s_agg, 10);
        const ns_board_v1_ApObservation *ap = &find(ns_board_v1_Record_ap_tag, 0)->kind.ap;
        CHECK_EQ(ap->beacon_count, 2);
        CHECK_EQ(ap->rssi_last, -50);
        CHECK_EQ(ap->security, ns_board_v1_SecurityType_SECURITY_TYPE_WPA2_WPA3);
        CHECK_EQ(ap->akm_suites_count, 2);
        CHECK_EQ(ap->phy_flags, ns_board_v1_PhyFlag_PHY_FLAG_HT | ns_board_v1_PhyFlag_PHY_FLAG_HE);
        CHECK_EQ(ap->channel_width, ns_board_v1_ChannelWidth_CHANNEL_WIDTH_MHZ_40);
        CHECK(ap->wps && ap->passpoint);
        CHECK_EQ(ap->supported_rates.size, 8);
        CHECK_EQ(ap->extended_supported_rates.size, 4);
        CHECK(ap->has_station_count);
        CHECK_EQ(ap->channel, 6);
        CHECK_EQ(ap->capability_info, 0x0411);
    }

    // Only cut frames: a cut before the RSN element says UNKNOWN, not OPEN or
    // WEP; a later complete frame sets everything.
    reset();
    aggregator_begin(&s_agg, 6, 0);
    feed_cut(&full, cuts[2], rx(6, -60, 1));
    aggregator_flush(&s_agg, 10);
    const ns_board_v1_ApObservation *ap = &find(ns_board_v1_Record_ap_tag, 0)->kind.ap;
    CHECK_EQ(ap->security, ns_board_v1_SecurityType_SECURITY_TYPE_UNKNOWN);
    CHECK_EQ(ap->ssid.size, 10);
    CHECK_EQ(ap->channel, 6);

    reset();
    aggregator_begin(&s_agg, 6, 0);
    feed_cut(&full, cuts[2], rx(6, -60, 1));
    feed_cut(&full, cuts[ncut - 1], rx(6, -60, 2));  // has RSN, cut before WPS
    CHECK_EQ(s_agg.aps[0].security, ns_board_v1_SecurityType_SECURITY_TYPE_WPA2_WPA3);
    CHECK(!s_agg.aps[0].wps);
    feed(&full, rx(6, -60, 3));
    aggregator_flush(&s_agg, 10);
    ap = &find(ns_board_v1_Record_ap_tag, 0)->kind.ap;
    CHECK(ap->wps && ap->passpoint);
    CHECK_EQ(ap->beacon_count, 3);

    // A beacon cut inside its fixed fields is not counted at all.
    reset();
    aggregator_begin(&s_agg, 6, 0);
    feed(&full, rx(6, -60, 1));
    feed_cut(&full, 30, rx(6, -40, 2));
    aggregator_flush(&s_agg, 10);
    ap = &find(ns_board_v1_Record_ap_tag, 0)->kind.ap;
    CHECK_EQ(ap->beacon_count, 1);
    CHECK_EQ(ap->rssi_max, -60);
    CHECK_EQ(ap->beacon_interval_tu, 100);
    CHECK_EQ(ap->capability_info, 0x0411);
    reset();
    aggregator_begin(&s_agg, 6, 0);
    feed_cut(&full, 30, rx(6, -40, 2));
    aggregator_flush(&s_agg, 10);
    CHECK_EQ(s_out_count, 0);
}

static void test_hidden_reveal_first(void)
{
    // The probe response arrives before the hidden beacon in the same dwell.
    reset();
    aggregator_begin(&s_agg, 1, 0);
    fb_t f;
    beacon(&f, 5, FB_AP1, "ExampleNet", 1);
    feed(&f, rx(1, -70, 1));
    beacon(&f, 8, FB_AP1, "", 1);
    feed(&f, rx(1, -70, 2));
    aggregator_flush(&s_agg, 10);
    const ns_board_v1_ApObservation *ap = &find(ns_board_v1_Record_ap_tag, 0)->kind.ap;
    CHECK(ap->ssid_hidden);
    CHECK_EQ(ap->ssid.size, 10);
}

static void test_multi_bssid_action(void)
{
    // AP2 beacons; an action frame it sends for a non-transmitted BSSID (address
    // 3 = AP1) has address 2 != address 3 but is not a client frame.
    reset();
    aggregator_begin(&s_agg, 1, 0);
    fb_t f;
    beacon(&f, 8, FB_AP2, "ExampleNet", 1);
    feed(&f, rx(1, -60, 1));
    fb_header(&f, FB_FC(0, 13), 0, FB_BCAST, FB_AP2, FB_AP1);
    fb_u8(&f, 4);
    feed(&f, rx(1, -60, 2));
    fb_header(&f, FB_FC(0, 11), 0, FB_STA1, FB_AP2, FB_AP1);
    fb_le16(&f, 0);
    fb_le16(&f, 2);
    fb_le16(&f, 0);
    feed(&f, rx(1, -60, 3));
    aggregator_flush(&s_agg, 10);
    CHECK_EQ(s_summary.aps, 1);
    CHECK_EQ(s_summary.clients, 0);
}

int main(void)
{
    test_ap_keying();
    test_ap_channel();
    test_hidden_ssid();
    test_clients();
    test_probes();
    test_ignored_frames();
    test_table_limits();
    test_dense_ap_fits();
    test_truncated_frames();
    test_hidden_reveal_first();
    test_multi_bssid_action();
    if (s_failures) {
        printf("aggregator: %d of %d checks FAILED\n", s_failures, s_checks);
        return 1;
    }
    printf("aggregator: OK (%d checks)\n", s_checks);
    return 0;
}
