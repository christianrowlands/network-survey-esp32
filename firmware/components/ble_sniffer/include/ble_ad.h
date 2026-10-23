// BLE advertising data (AD structures, Core Specification Supplement part A).
//
// Pure C with no ESP-IDF dependencies (host tested). Each AD structure is a
// length byte (covering the type byte and the data), a type byte and the
// data. A zero length ends the significant part (the rest is padding).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_AD_FLAGS 0x01
#define BLE_AD_NAME_SHORT 0x08
#define BLE_AD_NAME_COMPLETE 0x09
#define BLE_AD_TX_POWER 0x0A
#define BLE_AD_MANUFACTURER 0xFF

typedef struct {
    const uint8_t *name;  // Complete Local Name, else Shortened; points into the data
    uint8_t name_len;
    bool name_complete;
    bool has_company;     // First Manufacturer Specific Data with at least 2 data bytes
    uint16_t company;
    bool has_tx_power;
    int8_t tx_power;
    bool has_flags;
    uint8_t flags;
    size_t valid_len;     // Bytes covered by whole AD structures (before a zero length or a malformed one)
    bool malformed;       // A structure ran past the end of the data
} ble_ad_info_t;

// Walks the AD structures of data. Fields not present stay zero/false.
void ble_ad_parse(const uint8_t *data, size_t len, ble_ad_info_t *out);

// Length of the longest prefix of data, at most limit bytes, that ends on an
// AD structure boundary (a cut never splits a structure). Stops at a zero
// length or a malformed structure.
size_t ble_ad_cut(const uint8_t *data, size_t len, size_t limit);

#ifdef __cplusplus
}
#endif
