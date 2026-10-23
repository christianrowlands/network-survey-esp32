// Size budget helpers for the Records characteristic: a message never spans
// notifications, so a record that is too large for the negotiated MTU is
// trimmed of optional detail (docs/protocol.md section 4, "Fitting records").
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ns_board.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

// Framing around one Record sent alone in a Records notification: sequence (2),
// RecordBatch length varint (2), time_synced (2), records tag (1) and Record
// length varint (2).
#define NS_PROTO_RECORD_FRAME_OVERHEAD 9

// Largest encoded Record that fits alone in one Records notification.
size_t ns_proto_max_record_size(uint16_t att_mtu);

// Encoded size of rec, or 0 if it cannot be encoded.
size_t ns_proto_record_size(const ns_board_v1_Record *rec);

// Drops optional detail from rec, in the documented order, until it encodes in
// at most max_size bytes. Sets detail_trimmed on an ApObservation it changed.
// Returns false if it still does not fit; the caller then drops the record and
// counts it in BoardStatus.dropped_records.
bool ns_proto_fit_record(ns_board_v1_Record *rec, size_t max_size);

#ifdef __cplusplus
}
#endif
