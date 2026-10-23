#include "ns_proto_fit.h"

#include "pb_encode.h"

size_t ns_proto_max_record_size(uint16_t att_mtu) {
    const size_t overhead = 3 + NS_PROTO_RECORD_FRAME_OVERHEAD;  // 3 = ATT notification header
    return att_mtu > overhead ? att_mtu - overhead : 0;
}

size_t ns_proto_record_size(const ns_board_v1_Record *rec) {
    size_t size = 0;
    return pb_get_encoded_size(&size, ns_board_v1_Record_fields, rec) ? size : 0;
}

static bool fits(const ns_board_v1_Record *rec, size_t max_size) {
    size_t size = ns_proto_record_size(rec);
    return size > 0 && size <= max_size;
}

static void keep_suites(ns_board_v1_ApObservation *ap, pb_size_t n) {
    if (ap->pairwise_ciphers_count > n) ap->pairwise_ciphers_count = n;
    if (ap->akm_suites_count > n) ap->akm_suites_count = n;
}

// Trim steps for ApObservation, least useful detail first.
static void trim_ap_step(ns_board_v1_ApObservation *ap, int step) {
    switch (step) {
        case 0: ap->extended_supported_rates.size = 0; break;
        case 1: ap->has_station_count = false; ap->has_channel_utilization = false; break;
        case 2: keep_suites(ap, 2); break;
        case 3: ap->supported_rates.size = 0; break;
        case 4: keep_suites(ap, 1); break;
        default: break;
    }
}

#define AP_TRIM_STEPS 5

bool ns_proto_fit_record(ns_board_v1_Record *rec, size_t max_size) {
    if (fits(rec, max_size)) return true;
    if (rec->which_kind != ns_board_v1_Record_ap_tag) return false;

    ns_board_v1_ApObservation *ap = &rec->kind.ap;
    ap->detail_trimmed = true;
    for (int step = 0; step < AP_TRIM_STEPS; step++) {
        trim_ap_step(ap, step);
        if (fits(rec, max_size)) return true;
    }
    return false;
}
