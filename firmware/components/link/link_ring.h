// Bounded byte ring of variable-length entries (encoded records or raw
// frames). When full it drops the oldest entries, so producers never wait.
// Not thread-safe: the caller holds a lock. Pure C, host-testable.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Per-entry overhead: u16 length and a one-byte tag.
#define LINK_RING_ENTRY_HEADER 3

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;  // next write offset
    size_t tail;  // oldest entry offset
    size_t used;  // bytes in use, headers included
    uint32_t entries;
    uint32_t dropped;  // entries evicted to make room, plus entries too large to store
} link_ring_t;

void link_ring_init(link_ring_t *r, uint8_t *buf, size_t cap);

// Discards every entry without counting them as dropped.
void link_ring_clear(link_ring_t *r);

// Stores an entry, evicting the oldest ones as needed. Returns the number of
// entries evicted, or -1 if this entry can never fit (it is counted as
// dropped). An empty entry is ignored and returns 0.
int link_ring_push(link_ring_t *r, uint8_t tag, const uint8_t *data, size_t len);

// Tag and length of the oldest entry. False when empty.
bool link_ring_peek(const link_ring_t *r, uint8_t *tag, size_t *len);

// Copies the oldest entry into out and removes it. Returns its length, or 0
// when the ring is empty or out_cap is too small (the entry stays).
size_t link_ring_pop(link_ring_t *r, uint8_t *out, size_t out_cap, uint8_t *tag);

// Removes the oldest entry without copying it.
void link_ring_discard(link_ring_t *r);

// 0 to 100.
unsigned link_ring_fill_percent(const link_ring_t *r);

#ifdef __cplusplus
}
#endif
