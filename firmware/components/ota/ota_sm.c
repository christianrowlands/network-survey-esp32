#include "ota_sm.h"

#include <string.h>

void ota_sm_init(ota_sm_t *sm, uint32_t window_bytes)
{
    memset(sm, 0, sizeof(*sm));
    sm->window_bytes = window_bytes;
}

ota_sm_begin_t ota_sm_begin(ota_sm_t *sm, uint32_t image_size, const uint8_t sha256[OTA_SM_SHA256_LEN],
                            uint32_t slot_size, uint64_t now_ms)
{
    if (image_size == 0 || image_size > slot_size) {
        return OTA_SM_BEGIN_INVALID;
    }
    sm->last_activity_ms = now_ms;
    sm->gap_reported = false;
    if (sm->active && sm->image_size == image_size && memcmp(sm->sha256, sha256, OTA_SM_SHA256_LEN) == 0) {
        sm->acked_offset = sm->next_offset;
        return OTA_SM_BEGIN_RESUME;
    }
    sm->active = true;
    sm->image_size = image_size;
    memcpy(sm->sha256, sha256, OTA_SM_SHA256_LEN);
    sm->next_offset = 0;
    sm->acked_offset = 0;
    sm->idle_reported = false;
    sm->dup_acked = false;
    return OTA_SM_BEGIN_FRESH;
}

ota_sm_chunk_t ota_sm_chunk(ota_sm_t *sm, const uint8_t *data, size_t len, const uint8_t **bytes, size_t *n,
                            uint64_t now_ms)
{
    if (!sm->active) {
        if (sm->idle_reported) {
            return OTA_SM_CHUNK_IGNORE;
        }
        sm->idle_reported = true;
        return OTA_SM_CHUNK_NOT_ACTIVE;
    }
    if (len <= OTA_SM_OFFSET_LEN) {
        return OTA_SM_CHUNK_IGNORE;
    }
    sm->last_activity_ms = now_ms;
    uint32_t offset = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    const uint8_t *payload = data + OTA_SM_OFFSET_LEN;
    size_t count = len - OTA_SM_OFFSET_LEN;
    if (offset < sm->next_offset) {
        uint64_t end = (uint64_t)offset + count;
        if (end == sm->next_offset) {
            // The resend that ends where the board stands: the phone may have
            // lost an ack, so repeat it.
            return OTA_SM_CHUNK_ACK;
        }
        if (end < sm->next_offset) {
            // Other resends get the current position at most once a second.
            if (sm->dup_acked && now_ms - sm->dup_ack_ms < OTA_SM_DUP_ACK_INTERVAL_MS) {
                return OTA_SM_CHUNK_IGNORE;
            }
            sm->dup_acked = true;
            sm->dup_ack_ms = now_ms;
            return OTA_SM_CHUNK_ACK;
        }
        // Straddles next_offset (the phone changed its chunk size): keep the
        // new tail.
        uint32_t skip = sm->next_offset - offset;
        payload += skip;
        count -= skip;
        offset = sm->next_offset;
    }
    if (offset > sm->next_offset) {
        if (sm->gap_reported) {
            return OTA_SM_CHUNK_IGNORE;
        }
        sm->gap_reported = true;
        return OTA_SM_CHUNK_OUT_OF_ORDER;
    }
    sm->gap_reported = false;
    if ((uint64_t)offset + count > sm->image_size) {
        ota_sm_abort(sm);
        return OTA_SM_CHUNK_OVERFLOW;
    }
    *bytes = payload;
    *n = count;
    return OTA_SM_CHUNK_WRITE;
}

bool ota_sm_written(ota_sm_t *sm, size_t n)
{
    sm->next_offset += (uint32_t)n;
    if (sm->next_offset == sm->image_size || sm->next_offset - sm->acked_offset >= sm->window_bytes / 2) {
        sm->acked_offset = sm->next_offset;
        return true;
    }
    return false;
}

ota_sm_end_t ota_sm_end(const ota_sm_t *sm)
{
    if (!sm->active) {
        return OTA_SM_END_NOT_ACTIVE;
    }
    return sm->next_offset == sm->image_size ? OTA_SM_END_VERIFY : OTA_SM_END_INCOMPLETE;
}

bool ota_sm_hash_matches(const ota_sm_t *sm, const uint8_t digest[OTA_SM_SHA256_LEN])
{
    uint8_t diff = 0;
    for (int i = 0; i < OTA_SM_SHA256_LEN; i++) {
        diff |= (uint8_t)(sm->sha256[i] ^ digest[i]);
    }
    return diff == 0;
}

void ota_sm_abort(ota_sm_t *sm)
{
    sm->active = false;
    sm->next_offset = 0;
    sm->acked_offset = 0;
    sm->gap_reported = false;
    // The phone learns of an abort from its ack or command response; chunks
    // still in flight are dropped quietly.
    sm->idle_reported = true;
}

bool ota_sm_idle_expired(ota_sm_t *sm, uint64_t now_ms)
{
    if (!sm->active || now_ms - sm->last_activity_ms < OTA_SM_IDLE_TIMEOUT_MS) {
        return false;
    }
    ota_sm_abort(sm);
    sm->idle_reported = false;  // nobody was told
    return true;
}
