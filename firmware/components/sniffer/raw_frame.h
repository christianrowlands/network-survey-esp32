// Raw frame helpers for the sniffer: the data frame cut that keeps payload
// and key handshakes off the board, the rate limiter that keeps raw capture
// within its frame and byte budgets, and the legacy rate table.
//
// Pure C with no ESP-IDF dependencies (host tested in test/host/raw).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAW_LLC_SNAP_LEN 8            // AA AA 03, OUI (3), ethertype (2)
#define RAW_ETHERTYPE_EAPOL 0x888E     // 802.1X / EAPOL (4-way handshake, PMKID)
#define RAW_ETHERTYPE_PREAUTH 0x88C7   // RSN pre-authentication (EAPOL too)

// How many bytes of a data frame (802.11 type 2, without FCS) raw capture
// may keep: the MAC header plus at most the 8-byte LLC/SNAP header, never any
// payload. Returns 0 when the frame must not be kept at all: an EAPOL or RSN
// pre-authentication frame, or a frame too short to hold its own header.
// Protected frames, A-MSDUs and frames whose body does not start with an
// LLC/SNAP header keep the MAC header only (their ethertype cannot be read).
size_t raw_data_keep(const uint8_t *frame, size_t len);

// Token buckets for frames and bytes, refilled continuously, each holding at
// most one second of budget (the burst size). Starts full.
typedef struct {
    bool started;
    uint64_t last_us;
    uint64_t frame_credit;  // frames x 1e6
    uint64_t byte_credit;   // bytes x 1e6
} raw_limit_t;

// Takes one frame of `bytes` at now_us (monotonic) if both budgets allow it.
// A rate of 0 allows nothing. Budgets are passed on every call so a new
// Configure takes effect at once.
bool raw_limit_take(raw_limit_t *l, uint32_t frames_per_sec, uint32_t bytes_per_sec, uint32_t bytes,
                    uint64_t now_us);

// Legacy (DSSS/CCK and OFDM) rate in 500 kbps units for the receiver's 4-bit
// rate code: 0 to 3 long-preamble and 5 to 7 short-preamble DSSS/CCK, 8 to 15
// the OFDM RATE field of the L-SIG with R1 in bit 0 (0xB = 6 Mbps). 0 when the
// code is not a rate.
uint32_t raw_legacy_rate_500k(uint8_t code);

#ifdef __cplusplus
}
#endif
