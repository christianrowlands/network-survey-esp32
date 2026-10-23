#include "link_ring.h"

#include <string.h>

void link_ring_init(link_ring_t *r, uint8_t *buf, size_t cap)
{
    r->buf = buf;
    r->cap = cap;
    r->dropped = 0;
    link_ring_clear(r);
}

void link_ring_clear(link_ring_t *r)
{
    r->head = 0;
    r->tail = 0;
    r->used = 0;
    r->entries = 0;
}

static void copy_in(link_ring_t *r, size_t at, const uint8_t *src, size_t len)
{
    size_t first = r->cap - at;
    if (first > len) {
        first = len;
    }
    memcpy(r->buf + at, src, first);
    memcpy(r->buf, src + first, len - first);
}

static void copy_out(const link_ring_t *r, size_t at, uint8_t *dst, size_t len)
{
    size_t first = r->cap - at;
    if (first > len) {
        first = len;
    }
    memcpy(dst, r->buf + at, first);
    memcpy(dst + first, r->buf, len - first);
}

static size_t advance(const link_ring_t *r, size_t at, size_t n)
{
    at += n;
    return at >= r->cap ? at - r->cap : at;
}

bool link_ring_peek(const link_ring_t *r, uint8_t *tag, size_t *len)
{
    if (r->entries == 0) {
        return false;
    }
    uint8_t hdr[LINK_RING_ENTRY_HEADER];
    copy_out(r, r->tail, hdr, sizeof(hdr));
    *len = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
    if (tag != NULL) {
        *tag = hdr[2];
    }
    return true;
}

void link_ring_discard(link_ring_t *r)
{
    size_t len;
    if (!link_ring_peek(r, NULL, &len)) {
        return;
    }
    size_t total = LINK_RING_ENTRY_HEADER + len;
    r->tail = advance(r, r->tail, total);
    r->used -= total;
    r->entries--;
}

int link_ring_push(link_ring_t *r, uint8_t tag, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return 0;  // nothing to store; pop uses 0 for "empty"
    }
    size_t total = LINK_RING_ENTRY_HEADER + len;
    if (len > UINT16_MAX || total > r->cap) {
        r->dropped++;
        return -1;
    }
    int evicted = 0;
    while (r->cap - r->used < total) {
        link_ring_discard(r);
        evicted++;
    }
    const uint8_t hdr[LINK_RING_ENTRY_HEADER] = {(uint8_t)(len & 0xFF), (uint8_t)(len >> 8), tag};
    copy_in(r, r->head, hdr, sizeof(hdr));
    copy_in(r, advance(r, r->head, sizeof(hdr)), data, len);
    r->head = advance(r, r->head, total);
    r->used += total;
    r->entries++;
    r->dropped += (uint32_t)evicted;
    return evicted;
}

size_t link_ring_pop(link_ring_t *r, uint8_t *out, size_t out_cap, uint8_t *tag)
{
    size_t len;
    if (!link_ring_peek(r, tag, &len) || len > out_cap) {
        return 0;
    }
    copy_out(r, advance(r, r->tail, LINK_RING_ENTRY_HEADER), out, len);
    link_ring_discard(r);
    return len;
}

unsigned link_ring_fill_percent(const link_ring_t *r)
{
    if (r->cap == 0) {
        return 0;
    }
    return (unsigned)((r->used * 100 + r->cap - 1) / r->cap);
}
