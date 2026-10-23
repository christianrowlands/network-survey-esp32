// Raw frame capture for the Raw characteristic (docs/protocol.md section 9).
// The receive callback keeps up to the snaplen of each wanted frame in the
// sniffer's queue, within a frame and a byte budget, and the sniffer task
// turns it into a RawFrame for the sink.

#include <string.h>

#include "dot11_parser.h"
#include "raw_frame.h"
#include "sniffer.h"
#include "sniffer_internal.h"

#define RAW_DEFAULT_FPS 50
// Byte budget (captured bytes plus RawFrame fields) so raw capture leaves
// room for Records on a link that carries about 25 KB/s.
#define RAW_BYTES_PER_SEC (12 * 1024)
#define RAW_FIELDS_BYTES 32
#define RAW_TYPE_MGMT 0x1
#define RAW_TYPE_CTRL 0x2
#define RAW_TYPE_DATA 0x4

static sniffer_raw_sink_t s_sink;
static volatile uint32_t s_cfg_mask = RAW_TYPE_MGMT;  // from Configure
static volatile uint32_t s_mask;            // in effect, 0 = off
static volatile uint16_t s_snaplen;         // in effect, 0 = off
static volatile uint32_t s_fps = RAW_DEFAULT_FPS;
static raw_limit_t s_limit;                 // Wi-Fi task
static volatile uint32_t s_dropped;         // Wi-Fi task
static uint32_t s_dropped_task;             // sniffer task
static uint32_t s_dropped_reported;         // sniffer task
static ns_board_v1_RawFrame s_frame;        // sniffer task

void sniffer_set_raw_sink(const sniffer_raw_sink_t *sink)
{
    s_sink = *sink;
}

static uint32_t type_bit(wifi_promiscuous_pkt_type_t type)
{
    switch (type) {
    case WIFI_PKT_MGMT:
        return RAW_TYPE_MGMT;
    case WIFI_PKT_CTRL:
        return RAW_TYPE_CTRL;
    case WIFI_PKT_DATA:
        return RAW_TYPE_DATA;
    default:
        return 0;
    }
}

uint16_t sniffer_raw_take(wifi_promiscuous_pkt_type_t type, const uint8_t *frame, uint32_t len,
                          uint64_t now_us)
{
    uint16_t snap = s_snaplen;
    if (snap == 0 || (s_mask & type_bit(type)) == 0) {
        return 0;
    }
    if (type == WIFI_PKT_DATA) {
        // Never payload, never a key handshake (no EAPOL frame leaves the board).
        len = (uint32_t)raw_data_keep(frame, len);
        if (len == 0) {
            return 0;  // not a drop: these frames are never captured
        }
    }
    uint16_t keep = len < snap ? (uint16_t)len : snap;
    if (!raw_limit_take(&s_limit, s_fps, RAW_BYTES_PER_SEC, keep + RAW_FIELDS_BYTES, now_us)) {
        s_dropped++;
        return 0;
    }
    return keep;
}

void sniffer_raw_lost(void)
{
    s_dropped++;
}

void sniffer_raw_task_lost(void)
{
    s_dropped_task++;
}

// PHY, legacy rate and MCS. On HE capable chips the rate field is the 11b
// rate or the L-SIG RATE, and the SIG fields carry the MCS: HT-SIG1 bits 0 to
// 6, HE-SIG-A1 bits 3 to 6 (HE SU and ER SU).
void sniffer_raw_meta(const wifi_pkt_rx_ctrl_t *rx, rx_item_t *item)
{
    item->phy = ns_board_v1_FramePhy_FRAME_PHY_UNSPECIFIED;
    item->rate = 0;
    item->mcs = RAW_NO_MCS;
#if CONFIG_SOC_WIFI_HE_SUPPORT
    switch (rx->cur_bb_format) {
    case RX_BB_FORMAT_11B:
    case RX_BB_FORMAT_11G:
        item->phy = ns_board_v1_FramePhy_FRAME_PHY_LEGACY;
        item->rate = rx->rate;
        break;
    case RX_BB_FORMAT_HT:
        item->phy = ns_board_v1_FramePhy_FRAME_PHY_HT;
        item->mcs = rx->he_siga1 & 0x7F;
        break;
    case RX_BB_FORMAT_VHT:
    case RX_BB_FORMAT_VHT_MU:
        item->phy = ns_board_v1_FramePhy_FRAME_PHY_VHT;
        break;
    case RX_BB_FORMAT_HE_SU:
    case RX_BB_FORMAT_HE_ERSU:
        item->phy = ns_board_v1_FramePhy_FRAME_PHY_HE;
        item->mcs = (rx->he_siga1 >> 3) & 0x0F;
        break;
    case RX_BB_FORMAT_HE_MU:
    case RX_BB_FORMAT_HE_TB:
        item->phy = ns_board_v1_FramePhy_FRAME_PHY_HE;
        break;
    default:
        break;
    }
#else
    switch (rx->sig_mode) {
    case 0:
        item->phy = ns_board_v1_FramePhy_FRAME_PHY_LEGACY;
        item->rate = rx->rate;
        break;
    case 1:
        item->phy = ns_board_v1_FramePhy_FRAME_PHY_HT;
        item->mcs = rx->mcs;
        break;
    case 3:
        item->phy = ns_board_v1_FramePhy_FRAME_PHY_VHT;
        break;
    default:
        break;
    }
#endif
}

void sniffer_raw_configure(const ns_board_v1_Configure *cfg)
{
    uint32_t mask = cfg->raw_type_mask & (RAW_TYPE_MGMT | RAW_TYPE_CTRL | RAW_TYPE_DATA);
    s_cfg_mask = mask != 0 ? mask : RAW_TYPE_MGMT;
    s_fps = cfg->raw_max_frames_per_sec != 0 ? cfg->raw_max_frames_per_sec : RAW_DEFAULT_FPS;
}

void sniffer_raw_update(void)
{
    uint32_t snap = s_sink.snaplen != NULL ? s_sink.snaplen(s_sink.ctx) : 0;
    if (snap > sizeof(s_frame.data.bytes)) {
        snap = sizeof(s_frame.data.bytes);
    }
    s_mask = snap != 0 ? s_cfg_mask : 0;
    s_snaplen = (uint16_t)snap;
    uint32_t dropped = s_dropped + s_dropped_task;
    if (dropped != s_dropped_reported && s_sink.dropped != NULL) {
        s_sink.dropped(dropped - s_dropped_reported, s_sink.ctx);
        s_dropped_reported = dropped;
    }
}

uint32_t sniffer_raw_filter(void)
{
    uint32_t mask = s_snaplen != 0 ? s_mask : 0;
    return ((mask & RAW_TYPE_CTRL) ? WIFI_PROMIS_FILTER_MASK_CTRL : 0) |
           ((mask & RAW_TYPE_DATA) ? WIFI_PROMIS_FILTER_MASK_DATA : 0);
}

void sniffer_raw_emit(const rx_item_t *item, uint64_t now_us)
{
    if (s_sink.frame == NULL) {
        return;
    }
    ns_board_v1_RawFrame *f = &s_frame;
    memset(f, 0, sizeof(*f));
    f->board_time_us = now_us - (uint32_t)((uint32_t)now_us - item->time_us);
    f->channel = item->channel;
    f->frequency_mhz = dot11_channel_freq_mhz(item->channel);
    f->band = (ns_board_v1_Band)dot11_channel_band(item->channel);
    f->rssi = item->rssi;
    f->has_noise_floor_dbm = item->noise_floor != 0;
    f->noise_floor_dbm = item->noise_floor;
    f->phy = (ns_board_v1_FramePhy)item->phy;
    if (item->phy == ns_board_v1_FramePhy_FRAME_PHY_LEGACY) {
        f->rate_500kbps = raw_legacy_rate_500k(item->rate);
    }
    f->has_mcs = item->mcs != RAW_NO_MCS;
    f->mcs = f->has_mcs ? item->mcs : 0;
    f->orig_len = item->orig_len;
    f->fcs_included = false;  // the FCS is never kept
    uint16_t n = item->raw_len < item->len ? item->raw_len : item->len;
    if (n > sizeof(f->data.bytes)) {
        n = sizeof(f->data.bytes);
    }
    f->data.size = n;
    memcpy(f->data.bytes, item + 1, n);
    s_sink.frame(f, s_sink.ctx);
}
