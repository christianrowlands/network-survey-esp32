// OTA transfer state machine (docs/protocol.md section 10): which chunk to
// write, which to ignore and which ack to send. Pure C with no ESP-IDF
// dependencies (host tested in test/host/ota); ota.c does the flash writes,
// the SHA-256 and the acks.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_SM_SHA256_LEN 32
#define OTA_SM_OFFSET_LEN 4
// A transfer with no chunk or command for this long is discarded.
#define OTA_SM_IDLE_TIMEOUT_MS (5 * 60 * 1000)
// At most one ack per this interval for resent chunks below next_offset.
#define OTA_SM_DUP_ACK_INTERVAL_MS 1000

typedef struct {
    bool active;
    uint32_t image_size;
    uint8_t sha256[OTA_SM_SHA256_LEN];
    uint32_t next_offset;   // bytes written, contiguous from 0
    uint32_t window_bytes;  // most unacknowledged bytes the phone may send
    uint32_t acked_offset;  // next_offset of the last OK ack
    bool gap_reported;      // OUT_OF_ORDER sent; quiet until a chunk at next_offset
    bool idle_reported;     // NOT_ACTIVE sent, or the phone knows the transfer ended
    bool dup_acked;         // an ack answered a resent chunk, at dup_ack_ms
    uint64_t dup_ack_ms;
    uint64_t last_activity_ms;
} ota_sm_t;

typedef enum {
    OTA_SM_BEGIN_FRESH,    // start at 0; discard any partial image first
    OTA_SM_BEGIN_RESUME,   // same size and hash as the partial image: continue at next_offset
    OTA_SM_BEGIN_INVALID,  // size 0 or larger than the slot; nothing changed
} ota_sm_begin_t;

typedef enum {
    OTA_SM_CHUNK_WRITE,         // write the bytes at next_offset, then call ota_sm_written
    OTA_SM_CHUNK_IGNORE,        // resend (rate limited), repeat after a gap, or malformed
    OTA_SM_CHUNK_ACK,           // resend below next_offset: send OK with next_offset
    OTA_SM_CHUNK_OUT_OF_ORDER,  // send OUT_OF_ORDER
    OTA_SM_CHUNK_OVERFLOW,      // past image_size: transfer aborted, send OVERFLOW
    OTA_SM_CHUNK_NOT_ACTIVE,    // no transfer: send NOT_ACTIVE
} ota_sm_chunk_t;

typedef enum {
    OTA_SM_END_VERIFY,      // all bytes are in; compare the hash with ota_sm_hash_matches
    OTA_SM_END_NOT_ACTIVE,  // no transfer
    OTA_SM_END_INCOMPLETE,  // bytes missing
} ota_sm_end_t;

void ota_sm_init(ota_sm_t *sm, uint32_t window_bytes);

// OtaBegin. slot_size is the capacity of the update partition.
ota_sm_begin_t ota_sm_begin(ota_sm_t *sm, uint32_t image_size, const uint8_t sha256[OTA_SM_SHA256_LEN],
                            uint32_t slot_size, uint64_t now_ms);

// An OTA characteristic write: u32 LE offset, then image bytes. On
// OTA_SM_CHUNK_WRITE, *bytes and *n are the image bytes to write (the part
// past next_offset when the chunk starts below it).
ota_sm_chunk_t ota_sm_chunk(ota_sm_t *sm, const uint8_t *data, size_t len, const uint8_t **bytes, size_t *n,
                            uint64_t now_ms);

// The bytes from OTA_SM_CHUNK_WRITE are written. True when an OK ack is due
// (every window_bytes / 2 and at the last byte); the ack carries next_offset.
bool ota_sm_written(ota_sm_t *sm, size_t n);

// OtaEnd: whether the image is complete. The caller then compares hashes.
ota_sm_end_t ota_sm_end(const ota_sm_t *sm);
bool ota_sm_hash_matches(const ota_sm_t *sm, const uint8_t digest[OTA_SM_SHA256_LEN]);

// Discards the transfer (OtaAbort, write failure, verify failure, OtaEnd done).
void ota_sm_abort(ota_sm_t *sm);

// True once when an active transfer has been idle for OTA_SM_IDLE_TIMEOUT_MS;
// the transfer is then aborted.
bool ota_sm_idle_expired(ota_sm_t *sm, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
