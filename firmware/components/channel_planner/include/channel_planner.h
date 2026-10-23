// Channel list and dwell schedule from Configure.
//
// Pure logic with no ESP-IDF dependencies (host tested). The sniffer task asks
// for the next hop at every dwell boundary and reports how many APs it saw on
// the channel it just left; channels with recent APs get a longer dwell, up to
// twice the base dwell of their band.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ns_board.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CP_MAX_CHANNELS 64
#define CP_DEFAULT_DWELL_24_MS 250
#define CP_DEFAULT_DWELL_5_MS 150
#define CP_MIN_DWELL_MS 50
#define CP_MAX_DWELL_MS 5000

// Weighting: the busiest of the last CP_HISTORY visits to a channel sets its
// extra dwell, linear from 0 APs (base dwell) to CP_BUSY_APS or more (2x).
#define CP_HISTORY 4
#define CP_BUSY_APS 8

typedef struct {
    ns_board_v1_Role role;
    uint8_t count;                    // Channels in the schedule (0: no Wi-Fi)
    uint8_t channels[CP_MAX_CHANNELS];  // Hop order
    uint16_t dwell_24_ms;
    uint16_t dwell_5_ms;
    uint8_t next;                     // Index of the next hop
    uint8_t hist_pos[CP_MAX_CHANNELS];
    uint8_t hist[CP_MAX_CHANNELS][CP_HISTORY];  // AP counts of recent visits
} channel_planner_t;

typedef struct {
    uint8_t channel;
    uint32_t dwell_ms;
} cp_hop_t;

// True if the board can receive on this 20 MHz primary channel: 2.4 GHz 1 to
// 13, and on 5 GHz capable chips the 5 GHz primaries 36 to 177.
bool channel_planner_supported(uint32_t channel, bool supports_5g);

// Sets up the schedule the board starts with before any Configure: every
// supported channel, default dwells, ROLE_AUTO.
void channel_planner_init(channel_planner_t *p, bool supports_5g);

// Builds the schedule for cfg. Unsupported and duplicate channels are dropped;
// an empty list means every supported channel for the role. Returns false
// (and leaves p unchanged) when a Wi-Fi role ends up with no channel, which
// the board answers with INVALID_ARGUMENT. ROLE_BLE_SNIFFER gives an empty
// schedule and returns true.
bool channel_planner_configure(channel_planner_t *p, const ns_board_v1_Configure *cfg,
                               bool supports_5g);

// Next channel and its dwell. Returns false when the schedule is empty.
bool channel_planner_next(channel_planner_t *p, cp_hop_t *hop);

// Records how many APs one dwell on channel saw (weights later dwells).
void channel_planner_report(channel_planner_t *p, uint8_t channel, uint32_t ap_count);

#ifdef __cplusplus
}
#endif
