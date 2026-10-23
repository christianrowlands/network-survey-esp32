#include "aggregator.h"

#include <string.h>

#define MAC_LEN DOT11_MAC_LEN

void aggregator_init(aggregator_t *a, const aggregator_sink_t *sink)
{
    memset(a, 0, sizeof(*a));
    if (sink != NULL) {
        a->sink = *sink;
    }
    a->want_ap = true;
    a->want_client = true;
}

void aggregator_set_outputs(aggregator_t *a, bool ap, bool client, bool probes)
{
    a->want_ap = ap;
    a->want_client = client;
    a->want_probe = probes;
}

void aggregator_begin(aggregator_t *a, uint32_t channel, uint64_t now_ms)
{
    a->channel = channel;
    a->start_ms = now_ms;
    a->frames = 0;
    a->dropped = 0;
    a->ap_count = 0;
    a->client_count = 0;
    a->probe_count = 0;
}

static void update_rssi(int32_t *max, int32_t *last, bool first, int32_t rssi)
{
    if (first || rssi > *max) {
        *max = rssi;
    }
    *last = rssi;
}

// The AP's own channel from its elements, if it is a valid channel in the
// band the radio was tuned to; otherwise the receive channel.
static uint32_t ap_channel(const dot11_ies_t *ies, uint32_t rx_channel)
{
    uint32_t ie = ies->ds_channel != 0 ? ies->ds_channel : ies->ht_primary_channel;
    if (ie != 0 && dot11_channel_band(ie) == dot11_channel_band(rx_channel)) {
        return ie;
    }
    return rx_channel;
}

static ns_board_v1_ApObservation *find_ap(aggregator_t *a, const uint8_t *bssid)
{
    for (uint16_t i = 0; i < a->ap_count; i++) {
        if (memcmp(a->aps[i].bssid, bssid, MAC_LEN) == 0) {
            return &a->aps[i];
        }
    }
    return NULL;
}

// Security of a frame. A cut frame without RSN or WPA elements may have lost
// them, so it says nothing (rather than OPEN or WEP).
static ns_board_v1_SecurityType frame_security(const dot11_frame_t *f)
{
    if (f->truncated && !f->ies.rsn && !f->ies.wpa) {
        return ns_board_v1_SecurityType_SECURITY_TYPE_UNKNOWN;
    }
    return (ns_board_v1_SecurityType)f->security;
}

// Details of a complete frame: they describe the most recent frame.
static void set_details(ns_board_v1_ApObservation *o, const dot11_frame_t *f, uint32_t rx_channel)
{
    const dot11_ies_t *ies = &f->ies;
    o->channel = ap_channel(ies, rx_channel);
    o->security = frame_security(f);
    o->group_cipher = ies->group_cipher;
    o->pairwise_ciphers_count = ies->pairwise_count;
    memcpy(o->pairwise_ciphers, ies->pairwise, sizeof(uint32_t) * ies->pairwise_count);
    o->akm_suites_count = ies->akm_count;
    memcpy(o->akm_suites, ies->akm, sizeof(uint32_t) * ies->akm_count);
    o->phy_flags = ies->phy_flags;
    o->channel_width = (ns_board_v1_ChannelWidth)ies->width;
    o->wps = ies->wps;
    o->passpoint = ies->passpoint;
    o->supported_rates.size = ies->rates_len;
    memcpy(o->supported_rates.bytes, ies->rates, ies->rates_len);
    o->extended_supported_rates.size = ies->ext_rates_len;
    memcpy(o->extended_supported_rates.bytes, ies->ext_rates, ies->ext_rates_len);
    o->has_station_count = ies->bss_load;
    o->station_count = ies->station_count;
    o->has_channel_utilization = ies->bss_load;
    o->channel_utilization = ies->channel_utilization;
}

// Details of a cut frame (trailing elements missing): only fill what is still
// unknown and never lower what an earlier frame of the dwell reported.
static void merge_details(ns_board_v1_ApObservation *o, const dot11_frame_t *f, uint32_t rx_channel)
{
    const dot11_ies_t *ies = &f->ies;
    if (ies->ds_channel != 0 || ies->ht_primary_channel != 0) {
        o->channel = ap_channel(ies, rx_channel);
    }
    if (o->security == ns_board_v1_SecurityType_SECURITY_TYPE_UNKNOWN) {
        o->security = frame_security(f);
    }
    if (o->group_cipher == 0 && o->pairwise_ciphers_count == 0 && o->akm_suites_count == 0) {
        o->group_cipher = ies->group_cipher;
        o->pairwise_ciphers_count = ies->pairwise_count;
        memcpy(o->pairwise_ciphers, ies->pairwise, sizeof(uint32_t) * ies->pairwise_count);
        o->akm_suites_count = ies->akm_count;
        memcpy(o->akm_suites, ies->akm, sizeof(uint32_t) * ies->akm_count);
    }
    o->phy_flags |= ies->phy_flags;
    if (o->channel_width == ns_board_v1_ChannelWidth_CHANNEL_WIDTH_UNKNOWN) {
        o->channel_width = (ns_board_v1_ChannelWidth)ies->width;
    }
    o->wps = o->wps || ies->wps;
    o->passpoint = o->passpoint || ies->passpoint;
    if (o->supported_rates.size == 0) {
        o->supported_rates.size = ies->rates_len;
        memcpy(o->supported_rates.bytes, ies->rates, ies->rates_len);
    }
    if (o->extended_supported_rates.size == 0) {
        o->extended_supported_rates.size = ies->ext_rates_len;
        memcpy(o->extended_supported_rates.bytes, ies->ext_rates, ies->ext_rates_len);
    }
    if (!o->has_station_count && ies->bss_load) {
        o->has_station_count = true;
        o->station_count = ies->station_count;
        o->has_channel_utilization = true;
        o->channel_utilization = ies->channel_utilization;
    }
}

static void add_ap(aggregator_t *a, const dot11_frame_t *f, const aggregator_rx_t *rx)
{
    const uint8_t *bssid = f->addr3;
    // Without the fixed fields there is no interval or capability to report.
    if (dot11_is_group(bssid) || !f->fixed_fields) {
        return;
    }
    ns_board_v1_ApObservation *o = find_ap(a, bssid);
    bool first = o == NULL;
    if (first) {
        if (a->ap_count >= AGG_MAX_APS) {
            a->dropped++;
            return;
        }
        o = &a->aps[a->ap_count++];
        memset(o, 0, sizeof(*o));
        memcpy(o->bssid, bssid, MAC_LEN);
    }

    const dot11_ies_t *ies = &f->ies;
    o->board_time_ms = rx->time_ms;
    update_rssi(&o->rssi_max, &o->rssi_last, first, rx->rssi);
    o->has_noise_floor_dbm = rx->has_noise_floor;
    o->noise_floor_dbm = rx->noise_floor;
    if (f->kind == DOT11_KIND_BEACON) {
        o->beacon_count++;
    } else {
        o->probe_response_count++;
    }
    o->has_beacon_interval_tu = true;
    o->beacon_interval_tu = f->beacon_interval_tu;
    o->capability_info = f->capability;

    // A hidden network beacons an empty SSID but answers probes with the real
    // one: keep the first real SSID of the dwell and remember it was hidden.
    if (ies->ssid_present && ies->ssid_hidden) {
        o->ssid_hidden = true;
    } else if (ies->ssid_present && o->ssid.size == 0) {
        o->ssid.size = ies->ssid_len;
        memcpy(o->ssid.bytes, ies->ssid, ies->ssid_len);
    }

    if (first || !f->truncated) {
        set_details(o, f, rx->channel);
    } else {
        merge_details(o, f, rx->channel);
    }
    o->frequency_mhz = dot11_channel_freq_mhz(o->channel);
    o->band = (ns_board_v1_Band)dot11_channel_band(o->channel);
}

static void add_client(aggregator_t *a, const uint8_t *mac, const uint8_t *bssid, uint32_t source,
                       const aggregator_rx_t *rx)
{
    if (dot11_is_group(mac)) {
        return;
    }
    ns_board_v1_ClientObservation *o = NULL;
    for (uint16_t i = 0; i < a->client_count; i++) {
        if (memcmp(a->clients[i].client_mac, mac, MAC_LEN) == 0) {
            o = &a->clients[i];
            break;
        }
    }
    bool first = o == NULL;
    if (first) {
        if (a->client_count >= AGG_MAX_CLIENTS) {
            a->dropped++;
            return;
        }
        o = &a->clients[a->client_count++];
        memset(o, 0, sizeof(*o));
        memcpy(o->client_mac, mac, MAC_LEN);
        o->locally_administered = dot11_is_local(mac);
        o->channel = rx->channel;
        o->frequency_mhz = dot11_channel_freq_mhz(rx->channel);
        o->band = (ns_board_v1_Band)dot11_channel_band(rx->channel);
    }
    o->board_time_ms = rx->time_ms;
    update_rssi(&o->rssi_max, &o->rssi_last, first, rx->rssi);
    o->has_noise_floor_dbm = rx->has_noise_floor;
    o->noise_floor_dbm = rx->noise_floor;
    o->frame_count++;
    o->source_flags |= source;
    // Keep the last known BSS; frames without one do not clear it.
    if (bssid != NULL && !dot11_is_group(bssid) && memcmp(bssid, mac, MAC_LEN) != 0) {
        o->associated_bssid.size = MAC_LEN;
        memcpy(o->associated_bssid.bytes, bssid, MAC_LEN);
    }
}

static void add_probe(aggregator_t *a, const dot11_frame_t *f, const aggregator_rx_t *rx)
{
    const uint8_t *mac = f->addr2;
    const dot11_ies_t *ies = &f->ies;
    uint8_t ssid_len = ies->ssid_present ? ies->ssid_len : 0;
    if (dot11_is_group(mac)) {
        return;
    }
    ns_board_v1_ProbeObservation *o = NULL;
    for (uint16_t i = 0; i < a->probe_count; i++) {
        ns_board_v1_ProbeObservation *p = &a->probes[i];
        if (memcmp(p->client_mac, mac, MAC_LEN) == 0 && p->ssid.size == ssid_len &&
            memcmp(p->ssid.bytes, ies->ssid, ssid_len) == 0) {
            o = p;
            break;
        }
    }
    bool first = o == NULL;
    if (first) {
        if (a->probe_count >= AGG_MAX_PROBES) {
            a->dropped++;
            return;
        }
        o = &a->probes[a->probe_count++];
        memset(o, 0, sizeof(*o));
        memcpy(o->client_mac, mac, MAC_LEN);
        o->ssid.size = ssid_len;
        memcpy(o->ssid.bytes, ies->ssid, ssid_len);
        o->locally_administered = dot11_is_local(mac);
        o->channel = rx->channel;
        o->frequency_mhz = dot11_channel_freq_mhz(rx->channel);
        o->band = (ns_board_v1_Band)dot11_channel_band(rx->channel);
    }
    o->board_time_ms = rx->time_ms;
    update_rssi(&o->rssi_max, &o->rssi_last, first, rx->rssi);
    o->has_noise_floor_dbm = rx->has_noise_floor;
    o->noise_floor_dbm = rx->noise_floor;
    o->count++;
    o->destination_address.size = MAC_LEN;
    memcpy(o->destination_address.bytes, f->addr1, MAC_LEN);
    o->bssid.size = MAC_LEN;
    memcpy(o->bssid.bytes, f->addr3, MAC_LEN);
    o->phy_flags = ies->phy_flags;
}

// Management frames sent by a station rather than an AP: in a BSS the AP
// transmits with its BSSID as address 2, so address 2 != address 3 means a
// station sent it. Callers also skip transmitters already seen beaconing in
// the dwell (multiple-BSSID APs and AP-to-AP public action frames).
static bool from_station(const dot11_frame_t *f)
{
    return memcmp(f->addr2, f->addr3, MAC_LEN) != 0;
}

void aggregator_add(aggregator_t *a, const dot11_frame_t *f, const aggregator_rx_t *rx)
{
    a->frames++;
    switch (f->kind) {
    case DOT11_KIND_BEACON:
    case DOT11_KIND_PROBE_RESP:
        if (a->want_ap) {
            add_ap(a, f, rx);
        }
        break;
    case DOT11_KIND_PROBE_REQ:
        if (a->want_probe) {
            add_probe(a, f, rx);
        }
        if (a->want_client) {
            add_client(a, f->addr2, NULL, ns_board_v1_ClientSource_CLIENT_SOURCE_PROBE_REQ, rx);
        }
        break;
    case DOT11_KIND_ASSOC_REQ:
    case DOT11_KIND_REASSOC_REQ:
        if (a->want_client) {
            uint32_t src = f->kind == DOT11_KIND_ASSOC_REQ
                               ? ns_board_v1_ClientSource_CLIENT_SOURCE_ASSOC_REQ
                               : ns_board_v1_ClientSource_CLIENT_SOURCE_REASSOC_REQ;
            add_client(a, f->addr2, f->addr3, src, rx);
        }
        break;
    case DOT11_KIND_AUTH:
        if (a->want_client && from_station(f) && find_ap(a, f->addr2) == NULL) {
            add_client(a, f->addr2, f->addr3, ns_board_v1_ClientSource_CLIENT_SOURCE_AUTH, rx);
        }
        break;
    case DOT11_KIND_ACTION:
        if (a->want_client && from_station(f) && find_ap(a, f->addr2) == NULL) {
            add_client(a, f->addr2, NULL, ns_board_v1_ClientSource_CLIENT_SOURCE_ACTION, rx);
        }
        break;
    case DOT11_KIND_DATA:
    case DOT11_KIND_NULL:
        // Only frames the client transmitted: to the AP, or direct (IBSS).
        if (a->want_client && f->has_client &&
            (f->dir == DOT11_DIR_TO_AP || f->dir == DOT11_DIR_DIRECT)) {
            uint32_t src = f->kind == DOT11_KIND_DATA ? ns_board_v1_ClientSource_CLIENT_SOURCE_DATA
                                                      : ns_board_v1_ClientSource_CLIENT_SOURCE_NULL_DATA;
            add_client(a, f->client, f->has_bssid ? f->bssid : NULL, src, rx);
        }
        break;
    default:
        break;
    }
}

uint32_t aggregator_flush(aggregator_t *a, uint64_t now_ms)
{
    ns_board_v1_Record *r = &a->out;
    if (a->sink.record != NULL) {
        for (uint16_t i = 0; i < a->ap_count; i++) {
            r->which_kind = ns_board_v1_Record_ap_tag;
            r->kind.ap = a->aps[i];
            a->sink.record(r, a->sink.ctx);
        }
        for (uint16_t i = 0; i < a->client_count; i++) {
            r->which_kind = ns_board_v1_Record_client_tag;
            r->kind.client = a->clients[i];
            a->sink.record(r, a->sink.ctx);
        }
        for (uint16_t i = 0; i < a->probe_count; i++) {
            r->which_kind = ns_board_v1_Record_probe_tag;
            r->kind.probe = a->probes[i];
            a->sink.record(r, a->sink.ctx);
        }
    }
    uint32_t aps = a->ap_count;
    if (a->sink.dwell_done != NULL) {
        aggregator_dwell_summary_t s = {
            .channel = a->channel,
            .dwell_ms = (uint32_t)(now_ms - a->start_ms),
            .frames = a->frames,
            .aps = a->ap_count,
            .clients = a->client_count,
            .probes = a->probe_count,
            .dropped = a->dropped,
        };
        a->sink.dwell_done(&s, a->sink.ctx);
    }
    aggregator_begin(a, a->channel, now_ms);
    return aps;
}
