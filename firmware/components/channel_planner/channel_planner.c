#include "channel_planner.h"

#include <string.h>

// 20 MHz primaries of 5 GHz capable chips (ESP32-C5: 5180 to 5885 MHz).
static const uint8_t CH_5G[] = {
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165, 169, 173, 177,
};
#define CH_24_LAST 13  // The board's receive-only country config enables 1 to 13

static bool is_24(uint32_t ch) { return ch >= 1 && ch <= CH_24_LAST; }

static bool is_5(uint32_t ch)
{
    for (size_t i = 0; i < sizeof(CH_5G); i++) {
        if (CH_5G[i] == ch) {
            return true;
        }
    }
    return false;
}

bool channel_planner_supported(uint32_t channel, bool supports_5g)
{
    return is_24(channel) || (supports_5g && is_5(channel));
}

static bool role_allows(ns_board_v1_Role role, uint32_t ch)
{
    switch (role) {
    case ns_board_v1_Role_ROLE_BAND_24:
        return is_24(ch);
    case ns_board_v1_Role_ROLE_BAND_5:
        return !is_24(ch);
    default:
        return true;
    }
}

static void add(channel_planner_t *p, uint32_t ch)
{
    for (uint8_t i = 0; i < p->count; i++) {
        if (p->channels[i] == ch) {
            return;
        }
    }
    if (p->count < CP_MAX_CHANNELS) {
        p->channels[p->count++] = (uint8_t)ch;
    }
}

static uint16_t dwell_or_default(uint32_t ms, uint16_t def)
{
    if (ms == 0) {
        return def;
    }
    if (ms < CP_MIN_DWELL_MS) {
        return CP_MIN_DWELL_MS;
    }
    return (uint16_t)(ms > CP_MAX_DWELL_MS ? CP_MAX_DWELL_MS : ms);
}

void channel_planner_init(channel_planner_t *p, bool supports_5g)
{
    ns_board_v1_Configure cfg = ns_board_v1_Configure_init_zero;
    cfg.role = ns_board_v1_Role_ROLE_AUTO;
    memset(p, 0, sizeof(*p));
    channel_planner_configure(p, &cfg, supports_5g);
}

bool channel_planner_configure(channel_planner_t *p, const ns_board_v1_Configure *cfg,
                               bool supports_5g)
{
    channel_planner_t n;
    memset(&n, 0, sizeof(n));
    n.role = cfg->role;
    n.dwell_24_ms = dwell_or_default(cfg->dwell_24_ms, CP_DEFAULT_DWELL_24_MS);
    n.dwell_5_ms = dwell_or_default(cfg->dwell_5_ms, CP_DEFAULT_DWELL_5_MS);

    if (cfg->role == ns_board_v1_Role_ROLE_BLE_SNIFFER) {
        *p = n;
        return true;
    }
    if (cfg->role == ns_board_v1_Role_ROLE_FIXED_CHANNEL) {
        if (cfg->channels_count == 0 || !channel_planner_supported(cfg->channels[0], supports_5g)) {
            return false;
        }
        add(&n, cfg->channels[0]);
    } else if (cfg->channels_count > 0) {
        for (pb_size_t i = 0; i < cfg->channels_count && i < CP_MAX_CHANNELS; i++) {
            uint32_t ch = cfg->channels[i];
            if (channel_planner_supported(ch, supports_5g) && role_allows(cfg->role, ch)) {
                add(&n, ch);
            }
        }
    } else {
        for (uint32_t ch = 1; ch <= CH_24_LAST; ch++) {
            if (role_allows(cfg->role, ch)) {
                add(&n, ch);
            }
        }
        for (size_t i = 0; supports_5g && i < sizeof(CH_5G); i++) {
            if (role_allows(cfg->role, CH_5G[i])) {
                add(&n, CH_5G[i]);
            }
        }
    }
    if (n.count == 0) {
        return false;
    }
    *p = n;
    return true;
}

static int index_of(const channel_planner_t *p, uint8_t ch)
{
    for (uint8_t i = 0; i < p->count; i++) {
        if (p->channels[i] == ch) {
            return i;
        }
    }
    return -1;
}

bool channel_planner_next(channel_planner_t *p, cp_hop_t *hop)
{
    if (p->count == 0) {
        return false;
    }
    if (p->next >= p->count) {
        p->next = 0;
    }
    uint8_t i = p->next++;
    uint8_t ch = p->channels[i];
    uint32_t base = is_24(ch) ? p->dwell_24_ms : p->dwell_5_ms;
    uint32_t busy = 0;
    for (int k = 0; k < CP_HISTORY; k++) {
        busy = p->hist[i][k] > busy ? p->hist[i][k] : busy;
    }
    if (busy > CP_BUSY_APS) {
        busy = CP_BUSY_APS;
    }
    hop->channel = ch;
    hop->dwell_ms = p->count == 1 ? base : base + base * busy / CP_BUSY_APS;
    return true;
}

void channel_planner_report(channel_planner_t *p, uint8_t channel, uint32_t ap_count)
{
    int i = index_of(p, channel);
    if (i < 0) {
        return;
    }
    p->hist[i][p->hist_pos[i]] = (uint8_t)(ap_count > 255 ? 255 : ap_count);
    p->hist_pos[i] = (uint8_t)((p->hist_pos[i] + 1) % CP_HISTORY);
}
