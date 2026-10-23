#include "raw_frame.h"

#define MICRO 1000000ull

#define FC_TODS 0x01
#define FC_FROMDS 0x02
#define FC_PROTECTED 0x40
#define FC_ORDER 0x80  // +HTC in QoS data frames
#define SUBTYPE_NULL 0x04
#define SUBTYPE_QOS 0x08
#define QOS_AMSDU 0x80

size_t raw_data_keep(const uint8_t *frame, size_t len)
{
    if (len < 24 || ((frame[0] >> 2) & 3) != 2) {
        return 0;
    }
    uint8_t subtype = frame[0] >> 4;
    uint8_t flags = frame[1];
    size_t hdr = 24;
    if ((flags & (FC_TODS | FC_FROMDS)) == (FC_TODS | FC_FROMDS)) {
        hdr += 6;  // address 4
    }
    bool amsdu = false;
    if (subtype & SUBTYPE_QOS) {
        if (len < hdr + 2) {
            return 0;
        }
        amsdu = (frame[hdr] & QOS_AMSDU) != 0;
        hdr += 2;
        if (flags & FC_ORDER) {
            hdr += 4;  // HT control
        }
    }
    if (len < hdr) {
        return 0;
    }
    if ((subtype & SUBTYPE_NULL) || (flags & FC_PROTECTED) || amsdu) {
        return hdr;  // no body, or a body whose ethertype is not readable
    }
    const uint8_t *body = frame + hdr;
    size_t body_len = len - hdr;
    if (body_len < RAW_LLC_SNAP_LEN || body[0] != 0xAA || body[1] != 0xAA || body[2] != 0x03) {
        return hdr;
    }
    uint16_t ethertype = (uint16_t)((body[6] << 8) | body[7]);
    if (ethertype == RAW_ETHERTYPE_EAPOL || ethertype == RAW_ETHERTYPE_PREAUTH) {
        return 0;
    }
    return hdr + RAW_LLC_SNAP_LEN;
}

static uint64_t refill(uint64_t credit, uint64_t elapsed_us, uint32_t rate)
{
    uint64_t cap = (uint64_t)rate * MICRO;
    credit += elapsed_us * rate;
    return credit < cap ? credit : cap;
}

bool raw_limit_take(raw_limit_t *l, uint32_t frames_per_sec, uint32_t bytes_per_sec, uint32_t bytes,
                    uint64_t now_us)
{
    if (!l->started) {
        l->started = true;
        l->last_us = now_us;
        l->frame_credit = (uint64_t)frames_per_sec * MICRO;
        l->byte_credit = (uint64_t)bytes_per_sec * MICRO;
    }
    uint64_t elapsed = now_us > l->last_us ? now_us - l->last_us : 0;
    if (elapsed > MICRO) {
        elapsed = MICRO;  // a full bucket is one second of budget
    }
    l->last_us = now_us;
    l->frame_credit = refill(l->frame_credit, elapsed, frames_per_sec);
    l->byte_credit = refill(l->byte_credit, elapsed, bytes_per_sec);
    uint64_t need = (uint64_t)bytes * MICRO;
    if (l->frame_credit < MICRO || l->byte_credit < need) {
        return false;
    }
    l->frame_credit -= MICRO;
    l->byte_credit -= need;
    return true;
}

uint32_t raw_legacy_rate_500k(uint8_t code)
{
    static const uint8_t RATES[16] = {
        2, 4, 11, 22,      // 1, 2, 5.5, 11 Mbps, long preamble
        0, 4, 11, 22,      // short preamble (no 1 Mbps short)
        96, 48, 24, 12,    // 48, 24, 12, 6 Mbps
        108, 72, 36, 18,   // 54, 36, 18, 9 Mbps
    };
    return code < 16 ? RATES[code] : 0;
}
