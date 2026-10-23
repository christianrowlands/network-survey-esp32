// Host tests for the sniffer's raw frame helpers: the data frame cut (no
// payload, no EAPOL), the frame and byte rate limiter and the legacy rate
// table. Frames are synthetic (documentation-range and locally administered
// addresses, made-up nonce and PMKID bytes).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raw_frame.h"

static uint32_t s_rng = 0x9E3779B9u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int s_failures;
static int s_checks;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        s_checks++;                                                                      \
        if (!(cond)) {                                                                   \
            s_failures++;                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b) CHECK((long long)(a) == (long long)(b))

// Offers `offered` frames per second of `bytes` each for `seconds`, evenly
// spaced, starting at t0. Returns how many were taken.
static int offer(raw_limit_t *l, uint32_t fps, uint32_t bps, uint32_t bytes, int offered, int seconds,
                 uint64_t t0)
{
    int taken = 0;
    for (int i = 0; i < offered * seconds; i++) {
        uint64_t t = t0 + (uint64_t)i * 1000000u / (uint64_t)offered;
        taken += raw_limit_take(l, fps, bps, bytes, t);
    }
    return taken;
}

static void test_burst_and_refill(void)
{
    raw_limit_t l = {0};
    // A full bucket: 50 frames at once, then nothing until it refills.
    for (int i = 0; i < 50; i++) {
        CHECK(raw_limit_take(&l, 50, 1000000, 100, 1000));
    }
    CHECK(!raw_limit_take(&l, 50, 1000000, 100, 1000));
    CHECK(!raw_limit_take(&l, 50, 1000000, 100, 1000 + 19999));  // 1/50 s not yet over
    CHECK(raw_limit_take(&l, 50, 1000000, 100, 1000 + 20000));
    CHECK(!raw_limit_take(&l, 50, 1000000, 100, 1000 + 20000));
    // Time going backwards adds nothing.
    CHECK(!raw_limit_take(&l, 50, 1000000, 100, 500));
}

static void test_long_run(void)
{
    // Frame limited: 1000 offered per second for 10 s, 50 allowed per second
    // plus the initial one-second burst.
    raw_limit_t l = {0};
    int taken = offer(&l, 50, 1000000, 100, 1000, 10, 0);
    CHECK(taken >= 548 && taken <= 551);

    // Byte limited: 2000 B/s with 100-byte frames is 20 frames per second.
    raw_limit_t b = {0};
    taken = offer(&b, 1000, 2000, 100, 1000, 10, 0);
    CHECK(taken >= 218 && taken <= 221);

    // Under both limits nothing is dropped.
    raw_limit_t u = {0};
    CHECK_EQ(offer(&u, 50, 12288, 200, 40, 10, 0), 400);

    // A long idle gap refills at most one second of budget.
    raw_limit_t g = {0};
    offer(&g, 10, 1000000, 10, 100, 1, 0);
    taken = 0;
    for (int i = 0; i < 100; i++) {
        taken += raw_limit_take(&g, 10, 1000000, 10, 3600000000ull);
    }
    CHECK_EQ(taken, 10);
}

static void test_rate_change(void)
{
    raw_limit_t l = {0};
    CHECK_EQ(offer(&l, 100, 1000000, 10, 1000, 1, 0), 199);  // burst 100 + 99 refilled
    // Lowering the limit caps the stored credit at the new budget.
    int taken = 0;
    for (int i = 0; i < 1000; i++) {
        taken += raw_limit_take(&l, 5, 1000000, 10, 2000000);
    }
    CHECK(taken <= 5);
    // A rate of 0 allows nothing.
    raw_limit_t z = {0};
    CHECK_EQ(offer(&z, 0, 1000000, 10, 100, 2, 0), 0);
    raw_limit_t zb = {0};
    CHECK_EQ(offer(&zb, 100, 0, 10, 100, 2, 0), 0);
    // A frame larger than the whole byte budget never passes.
    raw_limit_t big = {0};
    CHECK(!raw_limit_take(&big, 100, 1000, 1001, 0));
    CHECK(raw_limit_take(&big, 100, 1000, 1000, 0));
}

static void test_rates(void)
{
    CHECK_EQ(raw_legacy_rate_500k(0), 2);    // 1 Mbps
    CHECK_EQ(raw_legacy_rate_500k(3), 22);   // 11 Mbps
    CHECK_EQ(raw_legacy_rate_500k(4), 0);    // no 1 Mbps short preamble
    CHECK_EQ(raw_legacy_rate_500k(6), 11);   // 5.5 Mbps short
    CHECK_EQ(raw_legacy_rate_500k(0xB), 12); // 6 Mbps (L-SIG 1101, R1 in bit 0)
    CHECK_EQ(raw_legacy_rate_500k(0xF), 18); // 9 Mbps (1111)
    CHECK_EQ(raw_legacy_rate_500k(0xA), 24); // 12 Mbps (0101)
    CHECK_EQ(raw_legacy_rate_500k(0xE), 36); // 18 Mbps (0111)
    CHECK_EQ(raw_legacy_rate_500k(0x9), 48); // 24 Mbps (1001)
    CHECK_EQ(raw_legacy_rate_500k(0xD), 72); // 36 Mbps (1011)
    CHECK_EQ(raw_legacy_rate_500k(0x8), 96); // 48 Mbps (0001)
    CHECK_EQ(raw_legacy_rate_500k(0xC), 108);// 54 Mbps (0011)
    CHECK_EQ(raw_legacy_rate_500k(16), 0);
    CHECK_EQ(raw_legacy_rate_500k(31), 0);
}

// ---------------------------------------------------------------------------
// Data frame cut

static const uint8_t AP[6] = {0x02, 0x00, 0x5E, 0x00, 0x00, 0x01};
static const uint8_t STA[6] = {0x0A, 0x00, 0x5E, 0x00, 0x00, 0x02};

// MAC header of a data frame: subtype (0 data, 4 null, 8 QoS data, 12 QoS
// null), flags byte, QoS control low byte when a QoS subtype. Returns length.
static size_t data_header(uint8_t *f, uint8_t subtype, uint8_t flags, uint8_t qos)
{
    size_t n = 0;
    f[n++] = (uint8_t)((subtype << 4) | (2 << 2));
    f[n++] = flags;
    f[n++] = 0x2C;  // duration
    f[n++] = 0x00;
    memcpy(f + n, STA, 6), n += 6;
    memcpy(f + n, AP, 6), n += 6;
    memcpy(f + n, AP, 6), n += 6;
    f[n++] = 0x10;  // sequence control
    f[n++] = 0x00;
    if ((flags & 0x03) == 0x03) {
        memcpy(f + n, STA, 6), n += 6;  // address 4
    }
    if (subtype & 0x08) {
        f[n++] = qos;
        f[n++] = 0x00;
        if (flags & 0x80) {
            memset(f + n, 0, 4), n += 4;  // HT control
        }
    }
    return n;
}

static size_t snap(uint8_t *f, uint16_t ethertype)
{
    const uint8_t h[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, (uint8_t)(ethertype >> 8), (uint8_t)ethertype};
    memcpy(f, h, 8);
    return 8;
}

// EAPOL-Key message 1 of the 4-way handshake carrying a PMKID KDE.
static size_t eapol_m1_pmkid(uint8_t *f)
{
    size_t n = 0;
    f[n++] = 0x02;  // 802.1X version
    f[n++] = 0x03;  // EAPOL-Key
    size_t len_at = n;
    n += 2;
    f[n++] = 0x02;  // RSN key descriptor
    f[n++] = 0x00;  // key information: pairwise, ACK, HMAC-SHA1/AES
    f[n++] = 0x8A;
    f[n++] = 0x00;  // key length 16
    f[n++] = 0x10;
    memset(f + n, 0, 7), n += 7;  // replay counter
    f[n++] = 0x01;
    memset(f + n, 0x11, 32), n += 32;  // ANonce (made up)
    memset(f + n, 0, 16 + 8 + 8 + 16), n += 16 + 8 + 8 + 16;  // IV, RSC, ID, MIC
    f[n++] = 0x00;  // key data length 22
    f[n++] = 0x16;
    const uint8_t kde[6] = {0xDD, 0x14, 0x00, 0x0F, 0xAC, 0x04};  // PMKID KDE
    memcpy(f + n, kde, 6), n += 6;
    memset(f + n, 0x22, 16), n += 16;  // PMKID (made up)
    f[len_at] = (uint8_t)((n - 4) >> 8);
    f[len_at + 1] = (uint8_t)(n - 4);
    return n;
}

static void test_eapol_never_kept(void)
{
    uint8_t f[256];
    // QoS data from the AP, plain data, 4-address, with HT control.
    const struct { uint8_t subtype, flags; } kinds[] = {{8, 0x02}, {0, 0x02}, {0, 0x01}, {8, 0x03}, {8, 0x82}};
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        size_t n = data_header(f, kinds[k].subtype, kinds[k].flags, 0);
        n += snap(f + n, RAW_ETHERTYPE_EAPOL);
        n += eapol_m1_pmkid(f + n);
        CHECK_EQ(raw_data_keep(f, n), 0);
        // Cut copies of the frame are never kept either once the ethertype is in.
        for (size_t cut = n - 1; cut >= n - 99 + 8; cut--) {
            CHECK_EQ(raw_data_keep(f, cut), 0);
        }
    }
    // RSN pre-authentication is EAPOL too.
    size_t n = data_header(f, 8, 0x01, 0);
    n += snap(f + n, RAW_ETHERTYPE_PREAUTH);
    n += eapol_m1_pmkid(f + n);
    CHECK_EQ(raw_data_keep(f, n), 0);
}

static void test_headers_only(void)
{
    uint8_t f[256];
    uint8_t payload[64];
    memset(payload, 0x45, sizeof(payload));
    // IPv4 over QoS data: MAC header (26) + LLC/SNAP (8), no payload.
    size_t n = data_header(f, 8, 0x01, 0);
    n += snap(f + n, 0x0800);
    memcpy(f + n, payload, sizeof(payload)), n += sizeof(payload);
    CHECK_EQ(raw_data_keep(f, n), 26 + 8);
    // Plain data: 24 + 8. 4-address: 30 + 8. QoS with HT control: 30 + 8.
    n = data_header(f, 0, 0x02, 0);
    n += snap(f + n, 0x86DD);
    memcpy(f + n, payload, 20), n += 20;
    CHECK_EQ(raw_data_keep(f, n), 24 + 8);
    n = data_header(f, 0, 0x03, 0);
    n += snap(f + n, 0x0806);
    memcpy(f + n, payload, 20), n += 20;
    CHECK_EQ(raw_data_keep(f, n), 30 + 8);
    n = data_header(f, 8, 0x81, 0);
    n += snap(f + n, 0x0800);
    memcpy(f + n, payload, 20), n += 20;
    CHECK_EQ(raw_data_keep(f, n), 30 + 8);

    // Protected: the body is not readable, so header only (an encrypted EAPOL
    // frame cannot leave either).
    n = data_header(f, 8, 0x41, 0);
    n += snap(f + n, RAW_ETHERTYPE_EAPOL);
    n += eapol_m1_pmkid(f + n);
    CHECK_EQ(raw_data_keep(f, n), 26);
    // A-MSDU: subframes start with addresses, header only even if one is EAPOL.
    n = data_header(f, 8, 0x02, 0x80);
    memcpy(f + n, STA, 6), n += 6;
    memcpy(f + n, AP, 6), n += 6;
    f[n++] = 0x00;
    f[n++] = 0x80;
    n += snap(f + n, RAW_ETHERTYPE_EAPOL);
    n += eapol_m1_pmkid(f + n);
    CHECK_EQ(raw_data_keep(f, n), 26);
    // Null and QoS null: header only.
    n = data_header(f, 4, 0x11, 0);
    CHECK_EQ(raw_data_keep(f, n), 24);
    n = data_header(f, 12, 0x11, 0);
    CHECK_EQ(raw_data_keep(f, n), 26);
    // Not LLC/SNAP, or a body too short for it: header only.
    n = data_header(f, 0, 0x01, 0);
    memcpy(f + n, payload, 20), n += 20;
    CHECK_EQ(raw_data_keep(f, n), 24);
    n = data_header(f, 0, 0x01, 0);
    f[n++] = 0xAA;
    f[n++] = 0xAA;
    f[n++] = 0x03;
    CHECK_EQ(raw_data_keep(f, n), 24);

    // Too short for its own header, or not a data frame: nothing.
    CHECK_EQ(raw_data_keep(f, 0), 0);
    CHECK_EQ(raw_data_keep(f, 23), 0);
    n = data_header(f, 8, 0x01, 0);
    CHECK_EQ(raw_data_keep(f, 25), 0);
    n = data_header(f, 8, 0x81, 0);
    CHECK_EQ(raw_data_keep(f, 29), 0);
    f[0] = 0x80;  // beacon
    CHECK_EQ(raw_data_keep(f, 100), 0);
    f[0] = 0xD4;  // ACK
    CHECK_EQ(raw_data_keep(f, 100), 0);
}

// Random frames: never more than the headers, never an unprotected EAPOL.
static void test_cut_fuzz(void)
{
    uint8_t f[128];
    int kept = 0;
    for (int iter = 0; iter < 200000; iter++) {
        size_t len = rnd() % sizeof(f);
        for (size_t i = 0; i < len; i++) {
            f[i] = (uint8_t)rnd();
        }
        if (len > 0 && iter % 2 == 0) {
            f[0] = (uint8_t)((f[0] & 0xF3) | 0x08);  // data type
        }
        if (len > 40 && iter % 4 == 0) {
            // Plant a SNAP EAPOL header at a plausible body offset.
            size_t at = 24 + (rnd() % 3) * 2;
            snap(f + at, RAW_ETHERTYPE_EAPOL);
        }
        uint8_t *heap = len ? malloc(len) : NULL;  // exact size for ASan
        if (len) {
            memcpy(heap, f, len);
        }
        size_t keep = raw_data_keep(heap, len);
        CHECK(keep <= len);
        if (keep > 0) {
            kept++;
            // Reference header length, written out independently.
            CHECK(((heap[0] >> 2) & 3) == 2);
            uint8_t st = heap[0] >> 4, fl = heap[1];
            size_t hdr = 24 + ((fl & 3) == 3 ? 6 : 0);
            bool amsdu = (st & 8) && (heap[hdr] & 0x80);
            hdr += (st & 8) ? 2 + ((fl & 0x80) ? 4 : 0) : 0;
            bool readable = !(st & 4) && !(fl & 0x40) && !amsdu;
            CHECK(keep == hdr || keep == hdr + RAW_LLC_SNAP_LEN);
            if (keep == hdr + RAW_LLC_SNAP_LEN) {
                CHECK(readable && heap[hdr] == 0xAA && heap[hdr + 1] == 0xAA && heap[hdr + 2] == 0x03);
                uint16_t et = (uint16_t)((heap[hdr + 6] << 8) | heap[hdr + 7]);
                CHECK(et != RAW_ETHERTYPE_EAPOL && et != RAW_ETHERTYPE_PREAUTH);
            } else if (readable && len >= hdr + RAW_LLC_SNAP_LEN && heap[hdr] == 0xAA && heap[hdr + 1] == 0xAA &&
                       heap[hdr + 2] == 0x03) {
                CHECK(0);  // a readable SNAP header must give hdr + 8 or nothing
            }
        }
        free(heap);
    }
    CHECK(kept > 30000);
}

int main(void)
{
    test_eapol_never_kept();
    test_headers_only();
    test_cut_fuzz();
    test_burst_and_refill();
    test_long_run();
    test_rate_change();
    test_rates();
    if (s_failures != 0) {
        printf("raw: %d of %d checks FAILED\n", s_failures, s_checks);
        return 1;
    }
    printf("raw: OK (%d checks)\n", s_checks);
    return 0;
}
