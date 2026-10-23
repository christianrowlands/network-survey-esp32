// Passive BLE advertisement capture (docs/protocol.md section 8,
// BleObservation). Scans with NimBLE's extended discovery, so both legacy and
// BLE 5 extended advertisements arrive, collects them per address over a
// report window (ble_agg.h) and hands one BleObservation per address and
// window to the sink. The board never sends scan requests.
//
// Modes (from Configure):
// - Wi-Fi roles with Configure.ble: a duty-cycled scan whose short windows
//   interleave with Wi-Fi reception on the shared radio.
// - ROLE_BLE_SNIFFER: a continuous scan (the sniffer stops Wi-Fi).
// Scanning runs alongside the phone connection and the board's advertising;
// the controller schedules scan windows around connection events (measured
// with an idle connection; not yet with Records streaming at full rate).
#pragma once

#include <stdint.h>

#include "ble_agg.h"
#include "esp_err.h"
#include "ns_board.pb.h"

// Report window: one observation per address per window.
#define BLE_SNIFFER_WINDOW_MS 1000

// Interleaved scan: BLE listens for WINDOW out of every INTERVAL (0.625 ms
// units). Measured on an ESP32-C5 hopping all channels: 20 of 100 ms kept 85
// to 89% of the AP records and saw about 60% of the advertisers a continuous
// scan sees per window; 30 ms saw about 40% more advertisers but kept only 77%
// of the AP records. Wi-Fi is the board's main job, so 20 ms.
#define BLE_SNIFFER_INTERLEAVE_ITVL 0x00A0    // 100 ms
#define BLE_SNIFFER_INTERLEAVE_WINDOW 0x0020  // 20 ms

typedef struct {
    uint32_t reports;          // Advertising reports received
    uint32_t dropped_reports;  // Lost because the report queue was full
    uint32_t observations;     // Records produced
    uint32_t windows;
    uint32_t scan_starts;      // Including restarts after an interruption
    uint32_t paused_windows;   // Windows cut short because NimBLE buffers ran low
} ble_sniffer_stats_t;

// Creates the sniffer task (idle until started). Call after gatt_server_init
// (the NimBLE host must exist; scanning begins once it has synced). The sink
// is called from the sniffer task.
esp_err_t ble_sniffer_init(ble_agg_record_fn record, void *ctx);

// Takes the BLE part of a configuration: continuous in ROLE_BLE_SNIFFER,
// interleaved when cfg->ble is set in a Wi-Fi role, otherwise off.
void ble_sniffer_configure(const ns_board_v1_Configure *cfg);

// Start and stop capture (idempotent). Stop flushes the open window.
esp_err_t ble_sniffer_start(void);
esp_err_t ble_sniffer_stop(void);

// dropped_reports goes into BoardStatus.dropped_rx_frames (main).
void ble_sniffer_get_stats(ble_sniffer_stats_t *out);
