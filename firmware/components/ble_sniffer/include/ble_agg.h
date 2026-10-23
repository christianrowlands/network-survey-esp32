// Per-window BLE advertiser table: one BleObservation per address per report
// window (docs/protocol.md section 8, BleObservation).
//
// Pure C with no ESP-IDF dependencies (host tested). The caller owns the entry
// array (so it can live in PSRAM) and the window timing. One table belongs to
// one task: calls are not thread safe.
//
// Per address the table keeps the last advertisement's PDU type and AD
// structures (cut to 31 bytes on a structure boundary), the strongest and the
// last RSSI and a count. Local name, company ID and TX power come from the
// latest advertisement in the window that carried them, so a device that
// alternates between two advertisements still reports its name.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ns_board.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_AGG_ADV_MAX 31   // BleObservation.adv_data limit
#define BLE_AGG_NAME_MAX 31  // BleObservation.local_name limit
#define BLE_AGG_NO_VALUE 127 // RSSI or TX power not available (HCI convention)

// Extended advertising report data status (HCI LE Extended Advertising Report).
typedef enum {
    BLE_AGG_DATA_COMPLETE = 0,
    BLE_AGG_DATA_MORE = 1,       // Incomplete, more reports follow
    BLE_AGG_DATA_TRUNCATED = 2,  // Incomplete, the rest was lost
} ble_agg_data_status_t;

// A report that follows one with BLE_AGG_DATA_MORE from the same address
// within this time continues that advertisement instead of starting one.
#define BLE_AGG_CONTINUE_MS 100

typedef struct {
    const uint8_t *addr;    // 6 bytes, printed order (most significant first)
    uint8_t addr_type;      // ns_board_v1_BleAddressType
    uint8_t adv_type;       // ns_board_v1_BleAdvType
    int8_t rssi;            // dBm, BLE_AGG_NO_VALUE if unknown
    int8_t tx_power;        // From the extended header, BLE_AGG_NO_VALUE if absent
    uint8_t data_status;    // ble_agg_data_status_t
    const uint8_t *data;    // AD structures of this report
    size_t len;
    uint64_t time_ms;       // Monotonic milliseconds since boot
} ble_agg_report_t;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    uint8_t adv_type;
    bool has_rssi;
    int8_t rssi_max;
    int8_t rssi_last;
    bool continuing;        // The last report said more data follows
    bool adv_truncated;
    bool has_company;
    bool has_tx_power;
    int8_t tx_power;
    uint16_t company;
    uint16_t count;
    uint8_t adv_len;
    uint8_t name_len;
    uint8_t adv[BLE_AGG_ADV_MAX];
    uint8_t name[BLE_AGG_NAME_MAX];
    uint64_t time_ms;
} ble_agg_entry_t;

// Receives one record per address during ble_agg_flush; the pointer is only
// valid during the call. board_time_ms is monotonic (the link converts it).
typedef void (*ble_agg_record_fn)(const ns_board_v1_Record *rec, void *ctx);

typedef struct {
    ble_agg_entry_t *entries;
    uint16_t capacity;
    uint16_t count;
    uint32_t reports;       // Reports added since the last flush
    ble_agg_record_fn record;
    void *ctx;
    ns_board_v1_Record out; // Scratch record handed to the sink
} ble_agg_t;

void ble_agg_init(ble_agg_t *a, ble_agg_entry_t *entries, uint16_t capacity, ble_agg_record_fn record,
                  void *ctx);

// Adds one advertising report. Returns false, and adds nothing, when the
// address is new and the table is full: flush, then add it again.
bool ble_agg_add(ble_agg_t *a, const ble_agg_report_t *r);

// Emits one record per address and clears the table. Returns the count.
uint32_t ble_agg_flush(ble_agg_t *a);

#ifdef __cplusplus
}
#endif
