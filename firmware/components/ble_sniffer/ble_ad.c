#include "ble_ad.h"

#include <string.h>

// Calls fn for every whole AD structure; returns the bytes they cover.
// *malformed is set when a structure runs past the end.
typedef void (*ad_fn)(uint8_t type, const uint8_t *data, uint8_t len, void *ctx);

static size_t walk(const uint8_t *data, size_t len, ad_fn fn, void *ctx, bool *malformed)
{
    size_t pos = 0;
    *malformed = false;
    while (pos < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0) {
            break;  // early termination, the rest is padding
        }
        if (pos + 1 + field_len > len) {
            *malformed = true;
            break;
        }
        if (fn != NULL) {
            fn(data[pos + 1], data + pos + 2, (uint8_t)(field_len - 1), ctx);
        }
        pos += 1 + (size_t)field_len;
    }
    return pos;
}

static void collect(uint8_t type, const uint8_t *d, uint8_t len, void *ctx)
{
    ble_ad_info_t *out = ctx;
    switch (type) {
    case BLE_AD_FLAGS:
        if (len >= 1 && !out->has_flags) {
            out->has_flags = true;
            out->flags = d[0];
        }
        break;
    case BLE_AD_NAME_COMPLETE:
        if (!out->name_complete) {
            out->name = d;
            out->name_len = len;
            out->name_complete = true;
        }
        break;
    case BLE_AD_NAME_SHORT:
        if (out->name == NULL) {
            out->name = d;
            out->name_len = len;
        }
        break;
    case BLE_AD_TX_POWER:
        if (len >= 1 && !out->has_tx_power) {
            out->has_tx_power = true;
            out->tx_power = (int8_t)d[0];
        }
        break;
    case BLE_AD_MANUFACTURER:
        if (len >= 2 && !out->has_company) {
            out->has_company = true;
            out->company = (uint16_t)(d[0] | (d[1] << 8));
        }
        break;
    default:
        break;
    }
}

void ble_ad_parse(const uint8_t *data, size_t len, ble_ad_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->valid_len = walk(data, len, collect, out, &out->malformed);
}

size_t ble_ad_cut(const uint8_t *data, size_t len, size_t limit)
{
    size_t pos = 0;
    while (pos < len) {
        uint8_t field_len = data[pos];
        size_t next = pos + 1 + (size_t)field_len;
        if (field_len == 0 || next > len || next > limit) {
            break;
        }
        pos = next;
    }
    return pos;
}
