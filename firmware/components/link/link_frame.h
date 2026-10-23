// Notification framing (docs/protocol.md section 4): a u16 little-endian
// sequence number followed by varint-delimited protobuf messages. A message
// never spans frames. Pure C with no ESP-IDF includes so host tests cover it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_FRAME_HEADER 2
// Largest notification payload: ATT MTU 517 minus the 3-byte ATT header.
#define LINK_FRAME_MAX 514
#define LINK_ATT_HEADER 3
// RecordBatch.records max_count (proto/ns_board.options).
#define LINK_BATCH_MAX_RECORDS 12

// Bytes needed to encode v as a base-128 varint (1 to 5).
size_t link_varint_size(uint32_t v);

// Writes v as a varint at out and returns the bytes written.
size_t link_varint_write(uint8_t *out, uint32_t v);

// Notification payload for an ATT MTU, capped at LINK_FRAME_MAX.
size_t link_frame_payload(uint16_t att_mtu);

// Largest message that fits alone in a frame of max_payload bytes.
size_t link_frame_max_message(size_t max_payload);

// Per-characteristic sequence counter. It restarts at 0 when the phone
// subscribes, and every frame the board tries to send uses a number.
typedef struct {
    uint16_t next;
} link_seq_t;

static inline void link_seq_reset(link_seq_t *s)
{
    s->next = 0;
}

static inline uint16_t link_seq_take(link_seq_t *s)
{
    return s->next++;  // wraps from 65535 to 0
}

// Rewrites the sequence number of a built frame (a frame kept for a retry
// after reconnecting gets a number from the new subscription).
static inline void link_frame_set_seq(uint8_t *frame, uint16_t seq)
{
    frame[0] = (uint8_t)(seq & 0xFF);
    frame[1] = (uint8_t)(seq >> 8);
}

// A frame being filled with messages. buf must hold max_payload bytes.
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    int count;
} link_frame_t;

void link_frame_init(link_frame_t *f, uint8_t *buf, size_t max_payload, uint16_t seq);

// Appends one length-delimited message. Returns false (frame unchanged) if it
// does not fit.
bool link_frame_add(link_frame_t *f, const uint8_t *msg, size_t len);

// A Records frame holding one RecordBatch that is built record by record from
// already encoded Record messages (protocol.md section 8, "Building batches").
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t batch_len;  // encoded RecordBatch bytes so far, starting at buf + 4
    int count;
    bool time_synced;
} link_batch_t;

void link_batch_init(link_batch_t *b, uint8_t *buf, size_t max_payload, uint16_t seq, bool time_synced);

// Size the batch would add for a Record of len bytes.
size_t link_batch_record_cost(size_t len);

// True if a Record of len bytes would still fit.
bool link_batch_can_add(const link_batch_t *b, size_t len);

// Appends an encoded Record. Returns false (batch unchanged) if it does not fit
// or the batch already holds LINK_BATCH_MAX_RECORDS.
bool link_batch_add(link_batch_t *b, const uint8_t *record, size_t len);

// Writes the RecordBatch length and returns the frame length (0 if empty).
size_t link_batch_finish(link_batch_t *b);

// Largest Record that fits alone in a Records frame of max_payload bytes.
size_t link_batch_max_record(size_t max_payload);

#ifdef __cplusplus
}
#endif
