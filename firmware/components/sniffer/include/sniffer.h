// Wi-Fi promiscuous receive and channel hopping.
//
// The driver callback only copies a capped prefix of each management and data
// frame plus its receive metadata into a ring buffer. The sniffer task tunes
// the radio per the channel planner, parses the queued frames and feeds the
// aggregator, which hands records to the sink at every dwell boundary. The
// radio never transmits: Wi-Fi runs in NULL mode with a receive-only country
// configuration.
#pragma once

#include <stdint.h>

#include "aggregator.h"
#include "esp_err.h"
#include "ns_board.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t rx_frames;          // Frames queued for parsing
    uint32_t dropped_rx_frames;  // Frames lost because the queue was full
    uint32_t truncated_frames;   // Management frames longer than the copy limit
    uint32_t channel_mismatch;   // Received on another channel than commanded (around a hop)
    uint32_t stale_frames;       // Arrived after their dwell was flushed
    uint32_t hops;
    uint32_t max_hop_us;         // Longest retune
    uint32_t max_frame_len;      // Longest frame seen, without FCS
    uint32_t agg_dropped;        // Frames lost because an aggregator table was full
    uint32_t stack_free_min;     // Sniffer task stack never used so far, bytes
} sniffer_stats_t;

// Starts Wi-Fi in NULL mode with the receive-only country config and creates
// the sniffer task, idle until sniffer_start. Needs NVS; creates the default
// event loop if it does not exist. The sink is called from the sniffer task.
esp_err_t sniffer_init(const aggregator_sink_t *sink);

// Raw frames (Configure.raw_frames, docs/protocol.md section 9). The sniffer
// asks snaplen() at every dwell boundary: 0 means raw frames are not wanted
// right now (raw off, not surveying, or nobody subscribed to Raw). Frames of
// the types in Configure.raw_type_mask are kept up to the snaplen, within
// Configure.raw_max_frames_per_sec and a byte budget; frames over either
// limit, or lost in the receive queue, are reported through dropped(). The
// callbacks run in the sniffer task.
typedef struct {
    void (*frame)(const ns_board_v1_RawFrame *frame, void *ctx);  // board_time_us monotonic
    uint32_t (*snaplen)(void *ctx);
    void (*dropped)(uint32_t count, void *ctx);
    void *ctx;
} sniffer_raw_sink_t;

// Call before sniffer_start.
void sniffer_set_raw_sink(const sniffer_raw_sink_t *sink);

// Validates cfg and schedules it for the next dwell boundary (or the next
// start). Data frames are parsed only while wifi_client is on. In
// ROLE_BLE_SNIFFER promiscuous mode is off (Wi-Fi stays started, see
// sniffer.c). Returns RESULT_CODE_OK and fills result (channels and role;
// the raw snaplen is left 0), or RESULT_CODE_INVALID_ARGUMENT when a Wi-Fi
// role has no receivable channel. result may be NULL.
ns_board_v1_ResultCode sniffer_configure(const ns_board_v1_Configure *cfg,
                                         ns_board_v1_ConfigureResult *result);

// Starts or stops capture. Stop is asynchronous: the task flushes the open
// dwell to the sink and turns promiscuous mode off.
esp_err_t sniffer_start(void);
esp_err_t sniffer_stop(void);

// Channel the radio listens on, 0 when not capturing.
uint32_t sniffer_current_channel(void);

void sniffer_get_stats(sniffer_stats_t *out);

// Bring-up aid (CONFIG_NS_SNIFFER_DEBUG_AUTOSTART): starts capture with a sink
// that logs per-dwell counts instead of sending records.
esp_err_t sniffer_debug_autostart(void);

#ifdef __cplusplus
}
#endif
