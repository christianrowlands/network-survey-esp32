// Per-dwell Wi-Fi observation tables, flushed into records at every channel
// change (docs/protocol.md section 8, "Aggregation").
//
// Pure C with no ESP-IDF dependencies (host tested). Fixed-size tables keyed
// by BSSID, client MAC and (client MAC, SSID); nothing is allocated. One
// aggregator belongs to one task: calls are not thread safe.
//
// Tables fill first come, first served: once a table is full, frames with new
// keys are counted in `dropped` and lost for that dwell, whatever their RSSI.
//
// Frames with dot11_frame_t.truncated set (cut capture) still count, but for
// an AP already in the table they only fill details that are still unknown,
// so a cut frame never replaces what a complete one reported.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dot11_parser.h"
#include "ns_board.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AGG_MAX_APS
#define AGG_MAX_APS 64
#endif
#ifndef AGG_MAX_CLIENTS
#define AGG_MAX_CLIENTS 64
#endif
#ifndef AGG_MAX_PROBES
#define AGG_MAX_PROBES 32
#endif

// Receive metadata of one frame.
typedef struct {
    uint32_t channel;  // Channel the radio was tuned to
    int32_t rssi;
    bool has_noise_floor;
    int32_t noise_floor;
    uint64_t time_ms;  // Monotonic milliseconds since boot
} aggregator_rx_t;

// What one dwell produced. Handed to the sink after the dwell's records.
typedef struct {
    uint32_t channel;
    uint32_t dwell_ms;      // From aggregator_begin to aggregator_flush
    uint32_t frames;        // Frames fed in, whatever their type
    uint16_t aps;
    uint16_t clients;
    uint16_t probes;
    uint16_t dropped;       // Frames with a new key that found its table full
} aggregator_dwell_summary_t;

// Receives the output. record() is called once per observation during
// aggregator_flush; the pointer is only valid during the call. Record times
// (board_time_ms) are monotonic ms since boot; the link converts them when it
// encodes a batch. Records are not fitted to the MTU here (ns_proto_fit_record
// is applied by the link, which knows the MTU).
typedef struct {
    void (*record)(const ns_board_v1_Record *rec, void *ctx);
    void (*dwell_done)(const aggregator_dwell_summary_t *summary, void *ctx);  // Optional
    void *ctx;
} aggregator_sink_t;

typedef struct {
    aggregator_sink_t sink;
    bool want_ap;
    bool want_client;
    bool want_probe;

    uint32_t channel;  // Channel of the open dwell
    uint64_t start_ms;
    uint32_t frames;
    uint16_t dropped;

    uint16_t ap_count;
    uint16_t client_count;
    uint16_t probe_count;
    ns_board_v1_ApObservation aps[AGG_MAX_APS];
    ns_board_v1_ClientObservation clients[AGG_MAX_CLIENTS];
    ns_board_v1_ProbeObservation probes[AGG_MAX_PROBES];
    ns_board_v1_Record out;  // Scratch record handed to the sink
} aggregator_t;

// Clears the tables. Outputs default to AP and client observations on, probes
// off (the board's settings before the first Configure).
void aggregator_init(aggregator_t *a, const aggregator_sink_t *sink);

// Which observation types to produce (Configure wifi_ap, wifi_client,
// wifi_probes). Takes effect for the frames that follow.
void aggregator_set_outputs(aggregator_t *a, bool ap, bool client, bool probes);

// Opens a dwell on channel. Call aggregator_flush first if one is open.
void aggregator_begin(aggregator_t *a, uint32_t channel, uint64_t now_ms);

// Adds one parsed frame to the open dwell.
void aggregator_add(aggregator_t *a, const dot11_frame_t *f, const aggregator_rx_t *rx);

// Emits one record per table entry, then the dwell summary, then clears the
// tables. Returns the number of APs in the dwell (for the channel planner).
uint32_t aggregator_flush(aggregator_t *a, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
