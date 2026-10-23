// Shared between the sniffer source files. Not part of the component API.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "ns_board.pb.h"

#define ITEM_PARSE 0x01      // feed the aggregator
#define ITEM_RAW 0x02        // send as a RawFrame
#define ITEM_TRUNCATED 0x04  // fewer bytes stored than the frame has

#define RAW_NO_MCS 0xFF

// Ring buffer item: this header, then len frame bytes. All fields are at most
// 4 bytes wide so items need no more than the ring's 4-byte alignment.
typedef struct {
    uint32_t gen;        // Dwell the frame arrived in
    uint32_t time_us;    // Low 32 bits of the monotonic clock
    uint16_t len;        // Bytes stored
    uint16_t orig_len;   // Frame length without the FCS
    uint16_t raw_len;    // Bytes for the RawFrame (ITEM_RAW)
    int8_t rssi;
    int8_t noise_floor;
    uint8_t channel;     // Channel commanded at receive time
    uint8_t rx_channel;  // Channel the driver reported
    uint8_t flags;       // ITEM_*
    uint8_t phy;         // ns_board_v1_FramePhy (ITEM_RAW)
    uint8_t rate;        // Legacy rate code (ITEM_RAW)
    uint8_t mcs;         // RAW_NO_MCS when unknown (ITEM_RAW)
} rx_item_t;

// Promiscuous receive callback side (Wi-Fi task). Returns the bytes to keep
// for a RawFrame, or 0 when this frame is not wanted raw (raw off, type not in
// Configure.raw_type_mask, an EAPOL data frame, or over the rate limit, which
// is counted). Data frames keep at most their MAC and LLC/SNAP headers.
uint16_t sniffer_raw_take(wifi_promiscuous_pkt_type_t type, const uint8_t *frame, uint32_t len,
                          uint64_t now_us);

// Fills the PHY, rate and MCS of item from the receive metadata.
void sniffer_raw_meta(const wifi_pkt_rx_ctrl_t *rx, rx_item_t *item);

// A frame wanted raw was lost: receive queue full (Wi-Fi task), or received
// on another channel than commanded (sniffer task).
void sniffer_raw_lost(void);
void sniffer_raw_task_lost(void);

// Sniffer task side.
void sniffer_raw_configure(const ns_board_v1_Configure *cfg);
// At dwell boundaries: asks the sink whether raw frames are wanted and at what
// snaplen, and reports the drops counted since the last call.
void sniffer_raw_update(void);
// Promiscuous filter bits raw capture needs (control and data frames).
uint32_t sniffer_raw_filter(void);
void sniffer_raw_emit(const rx_item_t *item, uint64_t now_us);

// sniffer_radio.c (sniffer task only).
esp_err_t radio_init(wifi_promiscuous_cb_t cb);  // NULL mode, receive-only country, started
void radio_set_filter(uint32_t mask);
uint32_t radio_tune(uint8_t channel);            // returns how long the retune took, microseconds
