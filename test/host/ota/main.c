// Host tests for the OTA transfer state machine (firmware/components/ota/
// ota_sm.c): in-order transfer and ack spacing, duplicates, gaps, a phone that
// overruns the board's queue, resume after a disconnect, size overrun, abort,
// idle timeout and the SHA-256 check. A randomized model at the end runs whole
// transfers over a lossy link with a phone that follows docs/protocol.md
// section 10 and checks the written image byte for byte.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota_sm.h"

static int failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4), small and slow, for the tests only.

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t i = 0;
    for (; i + 64 <= len; i += 64) {
        sha256_block(h, data + i);
    }
    uint8_t tail[128] = {0};
    size_t rest = len - i;
    memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    size_t tail_len = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int b = 0; b < 8; b++) {
        tail[tail_len - 1 - b] = (uint8_t)(bits >> (8 * b));
    }
    sha256_block(h, tail);
    if (tail_len == 128) {
        sha256_block(h, tail + 64);
    }
    for (int j = 0; j < 8; j++) {
        out[4 * j] = (uint8_t)(h[j] >> 24);
        out[4 * j + 1] = (uint8_t)(h[j] >> 16);
        out[4 * j + 2] = (uint8_t)(h[j] >> 8);
        out[4 * j + 3] = (uint8_t)h[j];
    }
}

static void test_sha256(void)
{
    // FIPS 180-2 example "abc".
    static const uint8_t abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                    0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                    0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    uint8_t d[32];
    sha256((const uint8_t *)"abc", 3, d);
    CHECK(memcmp(d, abc, 32) == 0);
}

// ---------------------------------------------------------------------------
// Helpers

#define SLOT (64 * 1024)
#define WINDOW 4096

static uint8_t image[SLOT];
static uint8_t flash[SLOT];
static uint8_t digest[32];

static void make_image(size_t size, unsigned seed)
{
    srand(seed);
    for (size_t i = 0; i < size; i++) {
        image[i] = (uint8_t)rand();
    }
    image[0] = 0xE9;  // ESP image magic, for realism only
    sha256(image, size, digest);
}

static size_t chunk(uint8_t *out, uint32_t offset, size_t n)
{
    out[0] = (uint8_t)offset;
    out[1] = (uint8_t)(offset >> 8);
    out[2] = (uint8_t)(offset >> 16);
    out[3] = (uint8_t)(offset >> 24);
    memcpy(out + 4, image + offset, n);
    return n + 4;
}

typedef struct {
    ota_sm_chunk_t result;
    bool ack_due;  // OK ack after the write
} step_t;

// Feeds one chunk and "writes" it to flash like ota.c does.
static step_t feed(ota_sm_t *sm, uint32_t offset, size_t n, uint64_t now)
{
    uint8_t buf[2048];
    size_t len = chunk(buf, offset, n);
    const uint8_t *bytes = NULL;
    size_t count = 0;
    step_t s = {ota_sm_chunk(sm, buf, len, &bytes, &count, now), false};
    if (s.result == OTA_SM_CHUNK_WRITE) {
        memcpy(flash + sm->next_offset, bytes, count);
        s.ack_due = ota_sm_written(sm, count);
    }
    return s;
}

static bool flash_hash_matches(const ota_sm_t *sm)
{
    uint8_t d[32];
    sha256(flash, sm->image_size, d);
    return ota_sm_hash_matches(sm, d);
}

// ---------------------------------------------------------------------------
// Directed tests

static void test_in_order(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    const size_t size = 10000, c = 500;
    make_image(size, 1);
    memset(flash, 0, sizeof(flash));
    CHECK(ota_sm_end(&sm) == OTA_SM_END_NOT_ACTIVE);
    CHECK(ota_sm_begin(&sm, size, digest, SLOT, 0) == OTA_SM_BEGIN_FRESH);
    CHECK(sm.next_offset == 0);
    int acks = 0;
    uint32_t last_ack = 0;
    for (uint32_t off = 0; off < size; off += c) {
        size_t n = size - off < c ? size - off : c;
        CHECK(ota_sm_end(&sm) == OTA_SM_END_INCOMPLETE);
        step_t s = feed(&sm, off, n, 1);
        CHECK(s.result == OTA_SM_CHUNK_WRITE);
        if (s.ack_due) {
            acks++;
            // At least every window/2 bytes.
            CHECK(sm.next_offset - last_ack >= WINDOW / 2 || sm.next_offset == size);
            CHECK(sm.next_offset - last_ack < WINDOW / 2 + c);
            last_ack = sm.next_offset;
        }
    }
    CHECK(sm.next_offset == size);
    CHECK(last_ack == size);  // the last byte is acked
    CHECK(acks == 4);         // 2500, 5000, 7500, 10000
    CHECK(ota_sm_end(&sm) == OTA_SM_END_VERIFY);
    CHECK(memcmp(flash, image, size) == 0);
    CHECK(flash_hash_matches(&sm));
}

static void test_duplicates(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    make_image(3000, 2);
    ota_sm_begin(&sm, 3000, digest, SLOT, 0);
    CHECK(feed(&sm, 0, 500, 0).result == OTA_SM_CHUNK_WRITE);
    CHECK(feed(&sm, 500, 500, 0).result == OTA_SM_CHUNK_WRITE);
    // The resend that ends exactly at next_offset always repeats the ack.
    CHECK(feed(&sm, 500, 500, 10).result == OTA_SM_CHUNK_ACK);
    CHECK(feed(&sm, 500, 500, 11).result == OTA_SM_CHUNK_ACK);
    // Other resends below next_offset: one ack per second at most.
    CHECK(feed(&sm, 0, 500, 100).result == OTA_SM_CHUNK_ACK);
    CHECK(feed(&sm, 0, 500, 200).result == OTA_SM_CHUNK_IGNORE);
    CHECK(feed(&sm, 100, 200, 100 + OTA_SM_DUP_ACK_INTERVAL_MS - 1).result == OTA_SM_CHUNK_IGNORE);
    CHECK(feed(&sm, 100, 200, 100 + OTA_SM_DUP_ACK_INTERVAL_MS).result == OTA_SM_CHUNK_ACK);
    CHECK(sm.next_offset == 1000);
    CHECK(feed(&sm, 1000, 500, 2000).result == OTA_SM_CHUNK_WRITE);
    CHECK(sm.next_offset == 1500);
    // Malformed: offset only, or shorter.
    uint8_t b[4] = {0xDC, 0x05, 0, 0};
    const uint8_t *bytes;
    size_t n;
    CHECK(ota_sm_chunk(&sm, b, 4, &bytes, &n, 0) == OTA_SM_CHUNK_IGNORE);
    CHECK(ota_sm_chunk(&sm, b, 2, &bytes, &n, 0) == OTA_SM_CHUNK_IGNORE);
    CHECK(sm.next_offset == 1500);
}

// A resend storm (a whole window resent at once) brings at most one ack.
static void test_duplicate_storm(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    make_image(60000, 12);
    ota_sm_begin(&sm, 60000, digest, SLOT, 0);
    for (uint32_t off = 0; off < 50000; off += 500) {
        feed(&sm, off, 500, 0);
    }
    int acks = 0;
    for (int i = 0; i < 100; i++) {
        acks += feed(&sm, (uint32_t)(i % 90) * 500, 500, 5000 + (uint64_t)i).result == OTA_SM_CHUNK_ACK;
    }
    CHECK(acks == 1);
    CHECK(sm.next_offset == 50000);
}

// A chunk that starts below next_offset and runs past it (the phone changed
// its chunk size) is trimmed: the new tail is written.
static void test_straddle(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    make_image(3000, 13);
    memset(flash, 0, sizeof(flash));
    ota_sm_begin(&sm, 3000, digest, SLOT, 0);
    CHECK(feed(&sm, 0, 500, 0).result == OTA_SM_CHUNK_WRITE);
    CHECK(feed(&sm, 400, 300, 0).result == OTA_SM_CHUNK_WRITE);
    CHECK(sm.next_offset == 700);
    CHECK(feed(&sm, 0, 1000, 0).result == OTA_SM_CHUNK_WRITE);
    CHECK(sm.next_offset == 1000);
    CHECK(feed(&sm, 1000, 1000, 0).result == OTA_SM_CHUNK_WRITE);
    // A straddling chunk past the image end still overflows.
    ota_sm_t sm2 = sm;
    CHECK(feed(&sm2, 1500, 1501, 0).result == OTA_SM_CHUNK_OVERFLOW);
    step_t last = feed(&sm, 1500, 1500, 0);
    CHECK(last.result == OTA_SM_CHUNK_WRITE && last.ack_due);
    CHECK(ota_sm_end(&sm) == OTA_SM_END_VERIFY);
    CHECK(memcmp(flash, image, 3000) == 0);
    CHECK(flash_hash_matches(&sm));
}

static void test_gap(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    make_image(4000, 3);
    memset(flash, 0, sizeof(flash));
    ota_sm_begin(&sm, 4000, digest, SLOT, 0);
    CHECK(feed(&sm, 0, 500, 0).result == OTA_SM_CHUNK_WRITE);
    // 500..1000 lost: OUT_OF_ORDER once, then quiet for the rest of the burst.
    CHECK(feed(&sm, 1000, 500, 0).result == OTA_SM_CHUNK_OUT_OF_ORDER);
    CHECK(feed(&sm, 1500, 500, 0).result == OTA_SM_CHUNK_IGNORE);
    CHECK(feed(&sm, 2000, 500, 0).result == OTA_SM_CHUNK_IGNORE);
    CHECK(sm.next_offset == 500);
    // Resend from next_offset.
    CHECK(feed(&sm, 500, 500, 0).result == OTA_SM_CHUNK_WRITE);
    CHECK(feed(&sm, 1000, 500, 0).result == OTA_SM_CHUNK_WRITE);
    // A new gap is reported again.
    CHECK(feed(&sm, 2000, 500, 0).result == OTA_SM_CHUNK_OUT_OF_ORDER);
    CHECK(feed(&sm, 2500, 500, 0).result == OTA_SM_CHUNK_IGNORE);
    CHECK(feed(&sm, 1500, 500, 0).result == OTA_SM_CHUNK_WRITE);
    CHECK(feed(&sm, 2000, 1000, 0).result == OTA_SM_CHUNK_WRITE);
    step_t last = feed(&sm, 3000, 1000, 0);
    CHECK(last.result == OTA_SM_CHUNK_WRITE && last.ack_due);
    CHECK(ota_sm_end(&sm) == OTA_SM_END_VERIFY);
    CHECK(flash_hash_matches(&sm));
}

// A phone that ignores the window overruns the board's chunk queue; the
// dropped chunks show up as a gap and the transfer still completes.
static void test_out_of_window(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    const size_t size = 20000, c = 400, queue = WINDOW * 2 / c;
    make_image(size, 4);
    memset(flash, 0, sizeof(flash));
    ota_sm_begin(&sm, size, digest, SLOT, 0);
    // Phone blasts the whole image; the board queue holds `queue` chunks.
    uint32_t sent = 0;
    for (size_t i = 0; i < queue; i++, sent += c) {
        CHECK(feed(&sm, sent, c, 0).result == OTA_SM_CHUNK_WRITE);
    }
    int out_of_order = 0;
    for (; sent < size; sent += c) {
        if ((sent / c - queue) % 3 == 0) {
            continue;  // queue full: dropped before the state machine sees it
        }
        out_of_order += feed(&sm, sent, c, 0).result == OTA_SM_CHUNK_OUT_OF_ORDER;
    }
    CHECK(out_of_order == 1);
    CHECK(sm.next_offset == queue * c);
    // Phone resends from next_offset, this time within the window.
    for (uint32_t off = sm.next_offset; off < size; off += c) {
        CHECK(feed(&sm, off, c, 0).result == OTA_SM_CHUNK_WRITE);
    }
    CHECK(ota_sm_end(&sm) == OTA_SM_END_VERIFY);
    CHECK(flash_hash_matches(&sm));
}

static void test_resume(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    make_image(5000, 5);
    memset(flash, 0, sizeof(flash));
    ota_sm_begin(&sm, 5000, digest, SLOT, 0);
    feed(&sm, 0, 1000, 0);
    feed(&sm, 1000, 1000, 0);
    CHECK(feed(&sm, 3000, 1000, 0).result == OTA_SM_CHUNK_OUT_OF_ORDER);  // gap, then the link drops
    // Same image: resume where the board stands.
    CHECK(ota_sm_begin(&sm, 5000, digest, SLOT, 10) == OTA_SM_BEGIN_RESUME);
    CHECK(sm.next_offset == 2000 && sm.acked_offset == 2000 && !sm.gap_reported);
    CHECK(feed(&sm, 2000, 1000, 0).result == OTA_SM_CHUNK_WRITE);
    // Resume once complete: start_offset is the image size, OtaEnd verifies.
    feed(&sm, 3000, 1000, 0);
    feed(&sm, 4000, 1000, 0);
    CHECK(ota_sm_begin(&sm, 5000, digest, SLOT, 20) == OTA_SM_BEGIN_RESUME);
    CHECK(sm.next_offset == 5000);
    CHECK(ota_sm_end(&sm) == OTA_SM_END_VERIFY);
    CHECK(flash_hash_matches(&sm));

    // Another image (size or hash) starts over.
    uint8_t other[32];
    memcpy(other, digest, 32);
    other[31] ^= 1;
    CHECK(ota_sm_begin(&sm, 5000, other, SLOT, 30) == OTA_SM_BEGIN_FRESH);
    CHECK(sm.next_offset == 0);
    feed(&sm, 0, 1000, 0);
    CHECK(ota_sm_begin(&sm, 4999, other, SLOT, 30) == OTA_SM_BEGIN_FRESH);
    CHECK(sm.next_offset == 0);
    // After an abort the same image starts over too.
    feed(&sm, 0, 1000, 0);
    ota_sm_abort(&sm);
    CHECK(ota_sm_begin(&sm, 4999, other, SLOT, 30) == OTA_SM_BEGIN_FRESH);
}

static void test_invalid_begin(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    make_image(1000, 6);
    CHECK(ota_sm_begin(&sm, 0, digest, SLOT, 0) == OTA_SM_BEGIN_INVALID);
    CHECK(ota_sm_begin(&sm, SLOT + 1, digest, SLOT, 0) == OTA_SM_BEGIN_INVALID);
    CHECK(!sm.active);
    CHECK(ota_sm_begin(&sm, SLOT, digest, SLOT, 0) == OTA_SM_BEGIN_FRESH);
    feed(&sm, 0, 500, 0);
    // An invalid OtaBegin leaves a running transfer alone.
    CHECK(ota_sm_begin(&sm, 0, digest, SLOT, 0) == OTA_SM_BEGIN_INVALID);
    CHECK(sm.active && sm.next_offset == 500);
}

static void test_overflow(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    make_image(1200, 7);
    ota_sm_begin(&sm, 1000, digest, SLOT, 0);
    CHECK(feed(&sm, 0, 500, 0).result == OTA_SM_CHUNK_WRITE);
    CHECK(feed(&sm, 500, 501, 0).result == OTA_SM_CHUNK_OVERFLOW);
    CHECK(!sm.active);
    // Chunks still in flight are dropped quietly; OtaEnd is refused.
    CHECK(feed(&sm, 1001, 100, 0).result == OTA_SM_CHUNK_IGNORE);
    CHECK(ota_sm_end(&sm) == OTA_SM_END_NOT_ACTIVE);
    // Exactly up to the end is fine.
    ota_sm_begin(&sm, 1000, digest, SLOT, 0);
    feed(&sm, 0, 500, 0);
    step_t s = feed(&sm, 500, 500, 0);
    CHECK(s.result == OTA_SM_CHUNK_WRITE && s.ack_due);
    CHECK(ota_sm_end(&sm) == OTA_SM_END_VERIFY);
}

static void test_abort_and_not_active(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    make_image(2000, 8);
    // A chunk before any OtaBegin: NOT_ACTIVE once.
    CHECK(feed(&sm, 0, 100, 0).result == OTA_SM_CHUNK_NOT_ACTIVE);
    CHECK(feed(&sm, 100, 100, 0).result == OTA_SM_CHUNK_IGNORE);
    ota_sm_begin(&sm, 2000, digest, SLOT, 0);
    feed(&sm, 0, 1000, 0);
    ota_sm_abort(&sm);
    CHECK(!sm.active && sm.next_offset == 0);
    CHECK(ota_sm_end(&sm) == OTA_SM_END_NOT_ACTIVE);
    CHECK(feed(&sm, 1000, 100, 0).result == OTA_SM_CHUNK_IGNORE);  // the phone asked for it
    ota_sm_abort(&sm);                                              // OtaAbort when idle is harmless
    CHECK(!sm.active);
}

static void test_idle_timeout(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    make_image(2000, 9);
    CHECK(!ota_sm_idle_expired(&sm, 10 * OTA_SM_IDLE_TIMEOUT_MS));
    ota_sm_begin(&sm, 2000, digest, SLOT, 1000);
    feed(&sm, 0, 500, 1000 + OTA_SM_IDLE_TIMEOUT_MS - 1);  // activity moves the deadline
    CHECK(!ota_sm_idle_expired(&sm, 1000 + 2 * OTA_SM_IDLE_TIMEOUT_MS - 2));
    CHECK(ota_sm_idle_expired(&sm, 1000 + 2 * OTA_SM_IDLE_TIMEOUT_MS - 1));
    CHECK(!sm.active);
    CHECK(!ota_sm_idle_expired(&sm, 1000 + 3 * OTA_SM_IDLE_TIMEOUT_MS));  // once
    // Nobody was told, so a stray chunk gets NOT_ACTIVE.
    CHECK(feed(&sm, 500, 500, 0).result == OTA_SM_CHUNK_NOT_ACTIVE);
}

static void test_sha_mismatch(void)
{
    ota_sm_t sm;
    ota_sm_init(&sm, WINDOW);
    make_image(3000, 10);
    memset(flash, 0, sizeof(flash));
    ota_sm_begin(&sm, 3000, digest, SLOT, 0);
    feed(&sm, 0, 1000, 0);
    image[1500] ^= 0x01;  // corrupted on the way
    feed(&sm, 1000, 1000, 0);
    image[1500] ^= 0x01;
    feed(&sm, 2000, 1000, 0);
    CHECK(ota_sm_end(&sm) == OTA_SM_END_VERIFY);
    CHECK(!flash_hash_matches(&sm));
    flash[1500] ^= 0x01;
    CHECK(flash_hash_matches(&sm));
    // Every byte of the expected hash counts.
    for (int i = 0; i < 32; i++) {
        uint8_t d[32];
        memcpy(d, digest, 32);
        d[i] ^= 0x80;
        CHECK(!ota_sm_hash_matches(&sm, d));
    }
}

// ---------------------------------------------------------------------------
// Randomized model: phone and board over a link that drops, duplicates and
// delays chunks, loses acks and disconnects. The phone keeps at most WINDOW
// unacknowledged bytes in flight, resends from next_offset on OUT_OF_ORDER and
// from its last ack after a timeout (sometimes with another chunk size), and
// resumes with the same OtaBegin after a disconnect. The board queue holds a
// bounded number of chunks. One step is one millisecond.

#define QCAP 64
#define BOARD_QUEUE 24

typedef struct {
    uint32_t offset;
    uint16_t n;
} msg_t;

typedef struct {
    uint32_t next_offset;
    int status;  // 1 OK, 2 OUT_OF_ORDER, 5 OVERFLOW
} ack_t;

static unsigned rnd(unsigned n)
{
    return (unsigned)rand() % n;
}

static void test_model(void)
{
    long total_steps = 0, total_resent = 0;
    for (int run = 0; run < 300; run++) {
        size_t size = 1 + rnd(SLOT);
        make_image(size, 100 + (unsigned)run);
        memset(flash, 0, sizeof(flash));
        size_t c = 20 + rnd(490);
        unsigned p_drop = rnd(8), p_dup = rnd(5), p_ack_loss = rnd(5), p_disc = rnd(3);

        ota_sm_t sm;
        ota_sm_init(&sm, WINDOW);
        CHECK(ota_sm_begin(&sm, (uint32_t)size, digest, SLOT, 0) == OTA_SM_BEGIN_FRESH);
        uint32_t acked = 0, send = 0;
        int idle = 0;
        msg_t q[QCAP];
        int qn = 0;
        ack_t acks[QCAP];
        int an = 0;
        long step = 0;
        uint64_t bytes_sent = 0;
        for (; step < 200000 && acked < size; step++) {
            // Phone sends one chunk when the window allows.
            if (send < size && send < acked + WINDOW && qn < QCAP) {
                uint16_t n = (uint16_t)(size - send < c ? size - send : c);
                if (rnd(100) >= p_drop) {
                    q[qn++] = (msg_t){send, n};
                    if (rnd(100) < p_dup && qn < QCAP) {
                        q[qn++] = (msg_t){send, n};
                    }
                }
                bytes_sent += n;
                send += n;
            }
            // Board takes one chunk (the queue overflows when it falls behind).
            if (qn > BOARD_QUEUE) {
                qn = BOARD_QUEUE;
            }
            if (qn > 0 && rnd(3) != 0) {
                msg_t m = q[0];
                memmove(q, q + 1, (size_t)(qn - 1) * sizeof(msg_t));
                qn--;
                step_t s = feed(&sm, m.offset, m.n, (uint64_t)step);
                int status = 0;
                if (s.result == OTA_SM_CHUNK_WRITE && s.ack_due) {
                    status = 1;
                } else if (s.result == OTA_SM_CHUNK_ACK) {
                    status = 1;
                } else if (s.result == OTA_SM_CHUNK_OUT_OF_ORDER) {
                    status = 2;
                }
                CHECK(s.result != OTA_SM_CHUNK_OVERFLOW && s.result != OTA_SM_CHUNK_NOT_ACTIVE);
                if (status != 0 && rnd(100) >= p_ack_loss && an < QCAP) {
                    acks[an++] = (ack_t){sm.next_offset, status};
                }
            }
            // Phone handles one ack.
            if (an > 0) {
                ack_t a = acks[0];
                memmove(acks, acks + 1, (size_t)(an - 1) * sizeof(ack_t));
                an--;
                CHECK(a.next_offset <= sm.next_offset);
                if (a.next_offset > acked) {
                    acked = a.next_offset;
                    idle = 0;
                }
                if (a.status == 2 || send < acked) {
                    send = a.next_offset > acked ? a.next_offset : acked;
                }
            } else if (++idle > 200) {
                send = acked;  // 5 s without an ack
                if (rnd(2) == 0) {
                    c = 20 + rnd(490);  // another chunk size: resends straddle next_offset
                }
                idle = 0;
            }
            // Disconnect: everything in flight is lost, then OtaBegin again.
            if (rnd(10000) < p_disc) {
                qn = an = 0;
                CHECK(ota_sm_begin(&sm, (uint32_t)size, digest, SLOT, 0) == OTA_SM_BEGIN_RESUME);
                acked = send = sm.next_offset;
                idle = 0;
            }
        }
        CHECK(acked == size);
        CHECK(ota_sm_end(&sm) == OTA_SM_END_VERIFY);
        CHECK(memcmp(flash, image, size) == 0);
        CHECK(flash_hash_matches(&sm));
        total_steps += step;
        total_resent += (long)(bytes_sent - size);
    }
    printf("ota model: 300 transfers, %ld steps, %ld bytes resent\n", total_steps, total_resent);
}

int main(void)
{
    test_sha256();
    test_in_order();
    test_duplicates();
    test_duplicate_storm();
    test_straddle();
    test_gap();
    test_out_of_window();
    test_resume();
    test_invalid_begin();
    test_overflow();
    test_abort_and_not_active();
    test_idle_timeout();
    test_sha_mismatch();
    test_model();
    if (failures != 0) {
        fprintf(stderr, "ota: %d check(s) failed\n", failures);
        return 1;
    }
    printf("ota: OK\n");
    return 0;
}
