#include "ble_agg.h"

#include <string.h>

#include "ble_ad.h"

void ble_agg_init(ble_agg_t *a, ble_agg_entry_t *entries, uint16_t capacity, ble_agg_record_fn record,
                  void *ctx)
{
    memset(a, 0, sizeof(*a));
    a->entries = entries;
    a->capacity = capacity;
    a->record = record;
    a->ctx = ctx;
}

static ble_agg_entry_t *find(ble_agg_t *a, const uint8_t *addr, uint8_t addr_type)
{
    for (uint16_t i = 0; i < a->count; i++) {
        ble_agg_entry_t *e = &a->entries[i];
        if (e->addr_type == addr_type && memcmp(e->addr, addr, 6) == 0) {
            return e;
        }
    }
    return NULL;
}

// A later chunk of an extended advertisement: the first chunk already gave
// the AD structures that fit in adv_data, so only the status matters.
static void continue_adv(ble_agg_entry_t *e, const ble_agg_report_t *r)
{
    e->continuing = r->data_status == BLE_AGG_DATA_MORE;
    e->adv_truncated = true;  // the advertisement did not fit in its first chunk
    e->time_ms = r->time_ms;
}

static void new_adv(ble_agg_entry_t *e, const ble_agg_report_t *r)
{
    e->count++;
    e->time_ms = r->time_ms;
    e->adv_type = r->adv_type;
    e->continuing = r->data_status == BLE_AGG_DATA_MORE;
    if (r->rssi != BLE_AGG_NO_VALUE) {
        if (!e->has_rssi || r->rssi > e->rssi_max) {
            e->rssi_max = r->rssi;
        }
        e->rssi_last = r->rssi;
        e->has_rssi = true;
    }

    ble_ad_info_t ad;
    ble_ad_parse(r->data, r->len, &ad);
    size_t keep = ble_ad_cut(r->data, r->len, BLE_AGG_ADV_MAX);
    memcpy(e->adv, r->data, keep);
    e->adv_len = (uint8_t)keep;
    e->adv_truncated = keep < ad.valid_len || ad.malformed || r->data_status != BLE_AGG_DATA_COMPLETE;

    if (ad.name != NULL && ad.name_len > 0) {
        e->name_len = ad.name_len < BLE_AGG_NAME_MAX ? ad.name_len : BLE_AGG_NAME_MAX;
        memcpy(e->name, ad.name, e->name_len);
    }
    if (ad.has_company) {
        e->has_company = true;
        e->company = ad.company;
    }
    if (ad.has_tx_power) {
        e->has_tx_power = true;
        e->tx_power = ad.tx_power;
    } else if (r->tx_power != BLE_AGG_NO_VALUE) {
        e->has_tx_power = true;
        e->tx_power = r->tx_power;
    }
}

bool ble_agg_add(ble_agg_t *a, const ble_agg_report_t *r)
{
    ble_agg_entry_t *e = find(a, r->addr, r->addr_type);
    if (e == NULL) {
        if (a->count >= a->capacity) {
            return false;
        }
        e = &a->entries[a->count++];
        memset(e, 0, sizeof(*e));
        memcpy(e->addr, r->addr, 6);
        e->addr_type = r->addr_type;
    } else if (e->continuing && r->time_ms - e->time_ms <= BLE_AGG_CONTINUE_MS) {
        a->reports++;
        continue_adv(e, r);
        return true;
    }
    a->reports++;
    new_adv(e, r);
    return true;
}

uint32_t ble_agg_flush(ble_agg_t *a)
{
    uint32_t n = a->count;
    for (uint16_t i = 0; i < a->count; i++) {
        const ble_agg_entry_t *e = &a->entries[i];
        ns_board_v1_Record *rec = &a->out;
        memset(rec, 0, sizeof(*rec));
        rec->which_kind = ns_board_v1_Record_ble_tag;
        ns_board_v1_BleObservation *o = &rec->kind.ble;
        o->board_time_ms = e->time_ms;
        memcpy(o->address, e->addr, 6);
        o->address_type = (ns_board_v1_BleAddressType)e->addr_type;
        o->rssi_max = e->has_rssi ? e->rssi_max : 0;
        o->rssi_last = e->has_rssi ? e->rssi_last : 0;
        o->count = e->count;
        o->adv_type = (ns_board_v1_BleAdvType)e->adv_type;
        o->adv_data.size = e->adv_len;
        memcpy(o->adv_data.bytes, e->adv, e->adv_len);
        o->adv_data_truncated = e->adv_truncated;
        o->local_name.size = e->name_len;
        memcpy(o->local_name.bytes, e->name, e->name_len);
        o->has_company_id = e->has_company;
        o->company_id = e->company;
        o->has_tx_power = e->has_tx_power;
        o->tx_power = e->tx_power;
        if (a->record != NULL) {
            a->record(rec, a->ctx);
        }
    }
    a->count = 0;
    a->reports = 0;
    return n;
}
