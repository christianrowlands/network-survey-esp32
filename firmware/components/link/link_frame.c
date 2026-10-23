#include "link_frame.h"

#include <string.h>

// RecordBatch field tags: time_synced (1, varint) and records (2, bytes).
#define TAG_TIME_SYNCED 0x08
#define TAG_RECORDS 0x12
// A batch never exceeds LINK_FRAME_MAX, so its length takes at most 2 bytes.
#define BATCH_LEN_RESERVE 2
#define BATCH_START (LINK_FRAME_HEADER + BATCH_LEN_RESERVE)

size_t link_varint_size(uint32_t v)
{
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

size_t link_varint_write(uint8_t *out, uint32_t v)
{
    size_t n = 0;
    while (v >= 0x80) {
        out[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

size_t link_frame_payload(uint16_t att_mtu)
{
    if (att_mtu <= LINK_ATT_HEADER) {
        return 0;
    }
    size_t payload = att_mtu - LINK_ATT_HEADER;
    return payload > LINK_FRAME_MAX ? LINK_FRAME_MAX : payload;
}

size_t link_frame_max_message(size_t max_payload)
{
    if (max_payload <= LINK_FRAME_HEADER + 1) {
        return 0;
    }
    size_t size = max_payload - LINK_FRAME_HEADER - 1;
    while (size > 0 && LINK_FRAME_HEADER + link_varint_size((uint32_t)size) + size > max_payload) {
        size--;
    }
    return size;
}

static void put_seq(uint8_t *buf, uint16_t seq)
{
    buf[0] = (uint8_t)(seq & 0xFF);
    buf[1] = (uint8_t)(seq >> 8);
}

void link_frame_init(link_frame_t *f, uint8_t *buf, size_t max_payload, uint16_t seq)
{
    f->buf = buf;
    f->cap = max_payload;
    f->len = LINK_FRAME_HEADER;
    f->count = 0;
    put_seq(buf, seq);
}

bool link_frame_add(link_frame_t *f, const uint8_t *msg, size_t len)
{
    size_t need = link_varint_size((uint32_t)len) + len;
    if (f->len + need > f->cap) {
        return false;
    }
    f->len += link_varint_write(f->buf + f->len, (uint32_t)len);
    memcpy(f->buf + f->len, msg, len);
    f->len += len;
    f->count++;
    return true;
}

// The batch is built at buf + 4 and moved left by one byte at the end if its
// length needs only one varint byte, so it must also fit from offset 4.
static bool batch_fits(size_t cap, size_t batch_len)
{
    return LINK_FRAME_HEADER + link_varint_size((uint32_t)batch_len) + batch_len <= cap &&
           BATCH_START + batch_len <= cap;
}

void link_batch_init(link_batch_t *b, uint8_t *buf, size_t max_payload, uint16_t seq, bool time_synced)
{
    b->buf = buf;
    b->cap = max_payload;
    b->batch_len = 0;
    b->count = 0;
    b->time_synced = time_synced;
    put_seq(buf, seq);
    if (time_synced) {
        buf[BATCH_START] = TAG_TIME_SYNCED;
        buf[BATCH_START + 1] = 1;
        b->batch_len = 2;
    }
}

size_t link_batch_record_cost(size_t len)
{
    return 1 + link_varint_size((uint32_t)len) + len;
}

bool link_batch_can_add(const link_batch_t *b, size_t len)
{
    return b->count < LINK_BATCH_MAX_RECORDS && batch_fits(b->cap, b->batch_len + link_batch_record_cost(len));
}

bool link_batch_add(link_batch_t *b, const uint8_t *record, size_t len)
{
    if (!link_batch_can_add(b, len)) {
        return false;
    }
    size_t grown = b->batch_len + link_batch_record_cost(len);
    uint8_t *p = b->buf + BATCH_START + b->batch_len;
    *p++ = TAG_RECORDS;
    p += link_varint_write(p, (uint32_t)len);
    memcpy(p, record, len);
    b->batch_len = grown;
    b->count++;
    return true;
}

size_t link_batch_finish(link_batch_t *b)
{
    if (b->count == 0) {
        return 0;
    }
    size_t vlen = link_varint_size((uint32_t)b->batch_len);
    uint8_t *start = b->buf + LINK_FRAME_HEADER;
    if (vlen < BATCH_LEN_RESERVE) {
        memmove(start + vlen, b->buf + BATCH_START, b->batch_len);
    }
    link_varint_write(start, (uint32_t)b->batch_len);
    return LINK_FRAME_HEADER + vlen + b->batch_len;
}

size_t link_batch_max_record(size_t max_payload)
{
    // Worst case: time_synced present (2 bytes) plus the records tag.
    if (max_payload <= BATCH_START + 4) {
        return 0;
    }
    size_t size = max_payload - BATCH_START - 4;
    while (size > 0 && !batch_fits(max_payload, 2 + link_batch_record_cost(size))) {
        size--;
    }
    return size;
}
