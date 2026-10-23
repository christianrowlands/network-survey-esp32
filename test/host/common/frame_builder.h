// Builds synthetic 802.11 frames for host tests. Addresses are locally
// administered or documentation style and SSIDs are made up: no captured
// traffic is used.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_MAX 1024

typedef struct {
    uint8_t buf[FB_MAX];
    size_t len;
} fb_t;

// Frame Control byte 0 for a type and subtype.
#define FB_FC(type, subtype) ((uint8_t)(((subtype) << 4) | ((type) << 2)))

static const uint8_t FB_BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t FB_AP1[6] = {0x02, 0x00, 0x5E, 0x10, 0x00, 0x01};
static const uint8_t FB_AP2[6] = {0x02, 0x00, 0x5E, 0x10, 0x00, 0x02};
static const uint8_t FB_STA1[6] = {0x0A, 0x00, 0x5E, 0x20, 0x00, 0x01};  // Locally administered
static const uint8_t FB_STA2[6] = {0x00, 0x00, 0x5E, 0x00, 0x53, 0x02};  // RFC 7042 documentation range

static inline void fb_bytes(fb_t *f, const void *p, size_t n)
{
    if (f->len + n > FB_MAX) {
        fprintf(stderr, "frame builder overflow\n");
        exit(2);
    }
    memcpy(f->buf + f->len, p, n);
    f->len += n;
}

static inline void fb_u8(fb_t *f, uint8_t v) { fb_bytes(f, &v, 1); }

static inline void fb_le16(fb_t *f, uint16_t v)
{
    fb_u8(f, (uint8_t)v);
    fb_u8(f, (uint8_t)(v >> 8));
}

// 24-byte header: Frame Control, duration, three addresses, sequence control.
static inline void fb_header(fb_t *f, uint8_t fc0, uint8_t flags, const uint8_t *a1,
                             const uint8_t *a2, const uint8_t *a3)
{
    f->len = 0;
    fb_u8(f, fc0);
    fb_u8(f, flags);
    fb_le16(f, 0);
    fb_bytes(f, a1, 6);
    fb_bytes(f, a2, 6);
    fb_bytes(f, a3, 6);
    fb_le16(f, 0x1230);
}

// Beacon (subtype 8) or probe response (5) header and fixed fields.
static inline void fb_beacon(fb_t *f, uint8_t subtype, const uint8_t *bssid, uint16_t capability)
{
    fb_header(f, FB_FC(0, subtype), 0, subtype == 8 ? FB_BCAST : FB_STA1, bssid, bssid);
    for (int i = 0; i < 8; i++) {
        fb_u8(f, (uint8_t)i);  // timestamp
    }
    fb_le16(f, 100);  // beacon interval
    fb_le16(f, capability);
}

static inline void fb_ie(fb_t *f, uint8_t id, const void *body, uint8_t n)
{
    fb_u8(f, id);
    fb_u8(f, n);
    fb_bytes(f, body, n);
}

static inline void fb_ssid(fb_t *f, const char *ssid)
{
    fb_ie(f, 0, ssid, (uint8_t)strlen(ssid));
}

// Element ID Extension element: 255, length, extension ID, body.
static inline void fb_ext(fb_t *f, uint8_t ext_id, const void *body, uint8_t n)
{
    fb_u8(f, 255);
    fb_u8(f, (uint8_t)(n + 1));
    fb_u8(f, ext_id);
    fb_bytes(f, body, n);
}

// RSN element body: version 1, group, pairwise list, AKM list, capabilities.
// Suites are given as 32-bit selectors, (OUI << 8) | type.
static inline uint8_t fb_suites_body(uint8_t *b, uint32_t group, const uint32_t *pw, int npw,
                                     const uint32_t *akm, int nakm)
{
    uint8_t n = 0;
    b[n++] = 1;
    b[n++] = 0;
    for (int s = 24; s >= 0; s -= 8) {
        b[n++] = (uint8_t)(group >> s);
    }
    b[n++] = (uint8_t)npw;
    b[n++] = 0;
    for (int i = 0; i < npw; i++) {
        for (int s = 24; s >= 0; s -= 8) {
            b[n++] = (uint8_t)(pw[i] >> s);
        }
    }
    b[n++] = (uint8_t)nakm;
    b[n++] = 0;
    for (int i = 0; i < nakm; i++) {
        for (int s = 24; s >= 0; s -= 8) {
            b[n++] = (uint8_t)(akm[i] >> s);
        }
    }
    return n;
}

static inline void fb_rsn(fb_t *f, uint32_t group, const uint32_t *pw, int npw, const uint32_t *akm,
                          int nakm)
{
    uint8_t b[200];
    uint8_t n = fb_suites_body(b, group, pw, npw, akm, nakm);
    b[n++] = 0x0C;  // RSN capabilities
    b[n++] = 0x00;
    fb_ie(f, 48, b, n);
}

static inline void fb_wpa(fb_t *f, uint32_t group, const uint32_t *pw, int npw, const uint32_t *akm,
                          int nakm)
{
    uint8_t b[200] = {0x00, 0x50, 0xF2, 0x01};
    uint8_t n = (uint8_t)(4 + fb_suites_body(b + 4, group, pw, npw, akm, nakm));
    fb_ie(f, 221, b, n);
}

// A 2.4 GHz beacon with every element the parser understands.
static inline void fb_full_beacon(fb_t *f)
{
    static const uint8_t rates[] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24};
    static const uint8_t ext_rates[] = {0x30, 0x48, 0x60, 0x6C};
    static const uint8_t ds[] = {6};
    static const uint8_t tim[] = {0, 1, 0, 0};
    static const uint8_t bss_load[] = {5, 0, 40, 0x00, 0x10};
    static const uint8_t ht_cap[26] = {0xEF, 0x01};
    static const uint8_t ht_op[22] = {6, 0x05};  // primary 6, secondary above, any width
    static const uint8_t interworking[] = {0x10};
    static const uint8_t hs20[] = {0x50, 0x6F, 0x9A, 0x10, 0x00};
    static const uint8_t wps[] = {0x00, 0x50, 0xF2, 0x04, 0x10, 0x4A, 0x00, 0x01, 0x10};
    static const uint8_t he_cap[22] = {0};
    const uint32_t pw[] = {0x000FAC04u};
    const uint32_t akm[] = {0x000FAC02u, 0x000FAC08u};

    fb_beacon(f, 8, FB_AP1, 0x0411);  // ESS, privacy, short slot
    fb_ssid(f, "ExampleNet");
    fb_ie(f, 1, rates, sizeof(rates));
    fb_ie(f, 3, ds, sizeof(ds));
    fb_ie(f, 5, tim, sizeof(tim));
    fb_ie(f, 11, bss_load, sizeof(bss_load));
    fb_ie(f, 45, ht_cap, sizeof(ht_cap));
    fb_rsn(f, 0x000FAC04u, pw, 1, akm, 2);
    fb_ie(f, 50, ext_rates, sizeof(ext_rates));
    fb_ie(f, 61, ht_op, sizeof(ht_op));
    fb_ie(f, 107, interworking, sizeof(interworking));
    fb_ext(f, 35, he_cap, sizeof(he_cap));
    fb_ie(f, 221, hs20, sizeof(hs20));
    fb_ie(f, 221, wps, sizeof(wps));
}
