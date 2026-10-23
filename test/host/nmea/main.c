// Host tests for the NMEA parser: line framing, checksums, GGA and RMC fields,
// GGA/RMC pairing into GpsFix, and garbage input. Sentences are synthetic;
// checksums are computed here.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nmea.h"
#include "ns_proto_fit.h"
#include "pb_encode.h"

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
#define CHECK_NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) <= (eps))

// "$" + body + "*hh".
static const char *sentence(const char *body)
{
    static char buf[4][256];
    static int next;
    char *out = buf[next++ % 4];
    unsigned sum = 0;
    for (const char *p = body; *p; p++) {
        sum ^= (unsigned char)*p;
    }
    snprintf(out, 256, "$%s*%02X", body, sum);
    return out;
}

// ---------------------------------------------------------------------------
// Line framing

static int feed_all(nmea_line_t *l, const char *bytes, char out[][NMEA_LINE_MAX + 1], int max)
{
    int n = 0;
    for (const char *p = bytes; *p; p++) {
        if (nmea_line_feed(l, (uint8_t)*p) && n < max) {
            strcpy(out[n++], l->buf);
        }
    }
    return n;
}

static void test_lines(void)
{
    nmea_line_t l;
    nmea_line_init(&l);
    char out[8][NMEA_LINE_MAX + 1];
    int n = feed_all(&l, "noise\r\n$GPGGA,1*00\r\n$GPRMC,2*00\n\n$GPGSV*00\r", out, 8);
    CHECK_EQ(n, 3);
    CHECK(strcmp(out[0], "$GPGGA,1*00") == 0);
    CHECK(strcmp(out[1], "$GPRMC,2*00") == 0);
    CHECK(strcmp(out[2], "$GPGSV*00") == 0);

    // "$" restarts a sentence; a control character drops it.
    n = feed_all(&l, "$GPGGA,cut$GPRMC,ok*00\r\n$GPGGA,\x01" "bad*00\r\n", out, 8);
    CHECK_EQ(n, 1);
    CHECK(strcmp(out[0], "$GPRMC,ok*00") == 0);

    // Over-long lines are dropped; the next sentence is fine.
    char longline[400] = "$GPGGA,";
    memset(longline + 7, '1', 300);
    strcat(longline, "\r\n$GPZDA*00\r\n");
    n = feed_all(&l, longline, out, 8);
    CHECK_EQ(n, 1);
    CHECK(strcmp(out[0], "$GPZDA*00") == 0);

    // A line of exactly NMEA_LINE_MAX characters fits.
    char exact[NMEA_LINE_MAX + 4] = "$";
    memset(exact + 1, 'A', NMEA_LINE_MAX - 1);
    exact[NMEA_LINE_MAX] = '\n';
    exact[NMEA_LINE_MAX + 1] = '\0';
    n = feed_all(&l, exact, out, 8);
    CHECK_EQ(n, 1);
    CHECK_EQ(strlen(out[0]), NMEA_LINE_MAX);
}

// ---------------------------------------------------------------------------
// Checksums and types

static void test_checksum(void)
{
    nmea_sentence_t s;
    const char *ok = sentence("GPGGA,120000.00,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    CHECK_EQ(nmea_parse(ok, &s), NMEA_GGA);

    char bad[256];
    strcpy(bad, ok);
    bad[strlen(bad) - 1] ^= 1;  // wrong last hex digit
    CHECK_EQ(nmea_parse(bad, &s), NMEA_INVALID);

    // Lower-case hex is accepted.
    strcpy(bad, ok);
    for (char *p = strchr(bad, '*'); *p; p++) {
        if (*p >= 'A' && *p <= 'F') {
            *p = (char)(*p - 'A' + 'a');
        }
    }
    CHECK_EQ(nmea_parse(bad, &s), NMEA_GGA);

    CHECK_EQ(nmea_parse("$GPGGA,120000.00,,,,,0,00,,,M,,M,,", &s), NMEA_INVALID);  // no checksum
    CHECK_EQ(nmea_parse("$GPGGA,1*", &s), NMEA_INVALID);
    CHECK_EQ(nmea_parse("$GPGGA,1*4", &s), NMEA_INVALID);
    CHECK_EQ(nmea_parse("$GPGGA,1*4G", &s), NMEA_INVALID);
    strcpy(bad, sentence("GPGGA,1"));
    strcat(bad, "0");  // trailing garbage after the checksum
    CHECK_EQ(nmea_parse(bad, &s), NMEA_INVALID);
    CHECK_EQ(nmea_parse("GPGGA,1*00", &s), NMEA_INVALID);  // no "$"
    CHECK_EQ(nmea_parse("", &s), NMEA_INVALID);
    CHECK_EQ(nmea_parse(sentence(""), &s), NMEA_INVALID);
    CHECK_EQ(nmea_parse(sentence("GPG"), &s), NMEA_INVALID);
    // Longer than a line can be: rejected (nmea_line_feed never produces one).
    char huge[200] = "GPGGA,";
    memset(huge + 6, '1', 150);
    CHECK_EQ(nmea_parse(sentence(huge), &s), NMEA_INVALID);

    // Talkers and other sentence types.
    CHECK_EQ(nmea_parse(sentence("GNGGA,120000,,,,,0,00,,,,,,,"), &s), NMEA_GGA);
    CHECK_EQ(nmea_parse(sentence("BDRMC,120000,V,,,,,,,221026,,,N"), &s), NMEA_RMC);
    CHECK_EQ(nmea_parse(sentence("GLRMC,120000,V,,,,,,,,,"), &s), NMEA_RMC);
    CHECK_EQ(nmea_parse(sentence("GPGSV,3,1,11,03,03,111,00"), &s), NMEA_OTHER);
    CHECK_EQ(nmea_parse(sentence("PUBX,00,120000.00"), &s), NMEA_OTHER);
    CHECK_EQ(nmea_parse(sentence("PMTKGGA,1"), &s), NMEA_OTHER);
    CHECK_EQ(nmea_parse(sentence("GPGGAX,1"), &s), NMEA_OTHER);
}

// ---------------------------------------------------------------------------
// Fields

static void test_gga(void)
{
    nmea_sentence_t s;
    CHECK_EQ(nmea_parse(sentence("GPGGA,123456.78,4807.0380,N,01131.0000,E,2,12,0.85,545.4,M,46.9,M,,"), &s),
             NMEA_GGA);
    CHECK(s.has_time);
    CHECK_EQ(s.time_ms, 12 * 3600000 + 34 * 60000 + 56780);
    CHECK(s.has_pos);
    CHECK_NEAR(s.lat, 48.0 + 7.038 / 60, 1e-9);
    CHECK_NEAR(s.lon, 11.0 + 31.0 / 60, 1e-9);
    CHECK_EQ(s.quality, 2);
    CHECK_EQ(s.satellites, 12);
    CHECK(s.has_hdop);
    CHECK_NEAR(s.hdop, 0.85, 1e-6);
    CHECK(s.has_alt);
    CHECK_NEAR(s.alt_m, 545.4, 1e-4);

    // Southern and western hemispheres, negative altitude, 3-digit longitude.
    CHECK_EQ(nmea_parse(sentence("GNGGA,000001,3352.1234,S,15112.5000,W,1,05,1.5,-12.5,M,,M,,"), &s), NMEA_GGA);
    CHECK_NEAR(s.lat, -(33.0 + 52.1234 / 60), 1e-9);
    CHECK_NEAR(s.lon, -(151.0 + 12.5 / 60), 1e-9);
    CHECK_NEAR(s.alt_m, -12.5, 1e-4);
    CHECK_EQ(s.time_ms, 1000);

    // No fix: empty fields.
    CHECK_EQ(nmea_parse(sentence("GPGGA,235959.00,,,,,0,00,99.99,,,,,,"), &s), NMEA_GGA);
    CHECK(s.has_time);
    CHECK(!s.has_pos);
    CHECK_EQ(s.quality, 0);
    CHECK(!s.has_alt);
    CHECK(s.has_hdop);

    // Truncated sentence: what is there still parses.
    CHECK_EQ(nmea_parse(sentence("GPGGA,120000,4807.038,N"), &s), NMEA_GGA);
    CHECK(s.has_time);
    CHECK(!s.has_pos);

    // Invalid values are not used.
    const char *bad[] = {
        "GPGGA,120000,4860.000,N,01131.000,E,1,08,0.9,1,M,,M,,",   // 60 minutes
        "GPGGA,120000,9100.000,N,01131.000,E,1,08,0.9,1,M,,M,,",   // 91 degrees
        "GPGGA,120000,4807.038,X,01131.000,E,1,08,0.9,1,M,,M,,",   // hemisphere
        "GPGGA,120000,4807.038,N,18100.000,E,1,08,0.9,1,M,,M,,",   // 181 degrees
        "GPGGA,120000,48A7.038,N,01131.000,E,1,08,0.9,1,M,,M,,",   // not a number
        "GPGGA,120000,4807.038,NS,01131.000,E,1,08,0.9,1,M,,M,,",  // two letters
        "GPGGA,120000,-4807.038,N,01131.000,E,1,08,0.9,1,M,,M,,",  // negative
        "GPGGA,120000,123456789012345678901234567890,N,01131.000,E,1,08,0.9,1,M,,M,,",  // huge
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_EQ(nmea_parse(sentence(bad[i]), &s), NMEA_GGA);
        CHECK(!s.has_pos);
    }
    // Bad times.
    const char *bad_time[] = {"GPGGA,246000,,,,,0,,,,,,,,", "GPGGA,126000,,,,,0,,,,,,,,", "GPGGA,1200,,,,,0,,,,,,,,",
                              "GPGGA,12a000,,,,,0,,,,,,,,"};
    for (size_t i = 0; i < sizeof(bad_time) / sizeof(bad_time[0]); i++) {
        CHECK_EQ(nmea_parse(sentence(bad_time[i]), &s), NMEA_GGA);
        CHECK(!s.has_time);
    }
    // Altitude without the unit letter is not used; satellites must be an integer.
    CHECK_EQ(nmea_parse(sentence("GPGGA,120000,4807.038,N,01131.000,E,1,8.5,0.9,545.4,,,,,"), &s), NMEA_GGA);
    CHECK(!s.has_alt);
    CHECK_EQ(s.satellites, 0);
    // Absurdly long numbers are rejected rather than overflowing.
    CHECK_EQ(nmea_parse(sentence("GPGGA,120000,4807.038,N,01131.000,E,1,08,12345678901234567890,"
                                 "99999999999999999999.0,M,,M,,"), &s), NMEA_GGA);
    CHECK(s.has_pos);
    CHECK(!s.has_hdop);
    CHECK(!s.has_alt);
}

static void test_rmc(void)
{
    nmea_sentence_t s;
    CHECK_EQ(nmea_parse(sentence("GPRMC,123456.789,A,4807.038,N,01131.000,E,10.0,084.4,221026,003.1,W,A"), &s),
             NMEA_RMC);
    CHECK(s.active);
    CHECK(s.has_time);
    CHECK_EQ(s.time_ms, 45296789);
    CHECK(s.has_pos);
    CHECK(s.has_speed);
    CHECK_NEAR(s.speed_mps, 5.14444, 1e-4);
    CHECK(s.has_course);
    CHECK_NEAR(s.course_deg, 84.4, 1e-4);
    CHECK(s.has_date);
    CHECK_EQ(s.year, 2026);
    CHECK_EQ(s.month, 10);
    CHECK_EQ(s.day, 22);

    CHECK_EQ(nmea_parse(sentence("GPRMC,000000,V,,,,,,,010100,,,N"), &s), NMEA_RMC);
    CHECK(!s.active);
    CHECK(!s.has_pos);
    CHECK(!s.has_speed);
    CHECK(s.has_date);
    CHECK_EQ(s.year, 2000);
    CHECK_EQ(nmea_parse(sentence("GPRMC,000000,V,,,,,,,060185,,,N"), &s), NMEA_RMC);
    CHECK_EQ(s.year, 1985);  // two-digit years 80 to 99 are the 1900s

    // Bad dates and courses.
    CHECK_EQ(nmea_parse(sentence("GPRMC,000000,A,,,,,0.0,360.0,321026,,,A"), &s), NMEA_RMC);
    CHECK(!s.has_date);
    CHECK(!s.has_course);
    CHECK(s.has_speed);
    CHECK_EQ(nmea_parse(sentence("GPRMC,000000,A,,,,,,,221326,,,A"), &s), NMEA_RMC);
    CHECK(!s.has_date);
    CHECK_EQ(nmea_parse(sentence("GPRMC,000000,A,,,,,,,22102,,,A"), &s), NMEA_RMC);
    CHECK(!s.has_date);
}

static void test_epoch(void)
{
    CHECK_EQ(nmea_epoch_ms(1970, 1, 1, 0), 0);
    CHECK_EQ(nmea_epoch_ms(2026, 10, 22, 45296789), 1792672496789ull);
    CHECK_EQ(nmea_epoch_ms(2000, 2, 29, 86399000), 951868799000ull);
    CHECK_EQ(nmea_epoch_ms(2099, 12, 31, 0), 4102358400000ull);
}

// ---------------------------------------------------------------------------
// Pairing

#define MAX_FIXES 8
static ns_board_v1_GpsFix s_fix[MAX_FIXES];
static int s_fixes;

static void on_fix(const ns_board_v1_GpsFix *fix, void *ctx)
{
    (void)ctx;
    if (s_fixes < MAX_FIXES) {
        s_fix[s_fixes++] = *fix;
    }
}

static void add(nmea_merge_t *m, const char *body, uint64_t now)
{
    nmea_sentence_t s;
    nmea_parse(sentence(body), &s);
    nmea_merge_add(m, &s, now);
}

#define GGA_1200 "GPGGA,120000.00,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"
#define RMC_1200 "GPRMC,120000.00,A,4807.038,N,01131.000,E,1.0,90.0,221026,,,A"
#define GGA_1201 "GPGGA,120001.00,4807.040,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"
#define RMC_1201 "GPRMC,120001.00,A,4807.040,N,01131.000,E,2.0,91.0,221026,,,A"

static void test_merge(void)
{
    nmea_merge_t m;
    uint64_t day = nmea_epoch_ms(2026, 10, 22, 0);

    // RMC first, then GGA (common order).
    s_fixes = 0;
    nmea_merge_init(&m, on_fix, NULL);
    add(&m, RMC_1200, 1000);
    CHECK_EQ(s_fixes, 0);
    add(&m, "GPGSA,A,3,04,05,,,,,,,,,,,1.5,0.9,1.2", 1001);
    add(&m, GGA_1200, 1002);
    CHECK_EQ(s_fixes, 1);
    CHECK_EQ(s_fix[0].board_time_ms, 1002);
    CHECK_EQ(s_fix[0].fix_time_ms, day + 12 * 3600000ull);
    CHECK_EQ(s_fix[0].fix_quality, 1);
    CHECK_EQ(s_fix[0].satellites, 8);
    CHECK(s_fix[0].has_speed_mps && s_fix[0].has_course_deg && s_fix[0].has_hdop && s_fix[0].has_altitude_m);
    CHECK_NEAR(s_fix[0].course_deg, 90.0, 1e-4);
    CHECK_NEAR(s_fix[0].latitude, 48.0 + 7.038 / 60, 1e-9);
    nmea_merge_poll(&m, 10000);
    CHECK_EQ(s_fixes, 1);  // nothing left waiting

    // GGA first, then RMC of the same second.
    add(&m, GGA_1201, 2000);
    CHECK_EQ(s_fixes, 1);
    add(&m, RMC_1201, 2005);
    CHECK_EQ(s_fixes, 2);
    CHECK_EQ(s_fix[1].board_time_ms, 2000);  // when the GGA arrived
    CHECK_EQ(s_fix[1].fix_time_ms, day + 12 * 3600000ull + 1000);
    CHECK_NEAR(s_fix[1].speed_mps, 2.0 * 0.514444, 1e-4);

    // GGA without RMC: sent alone once NMEA_PAIR_MS passed, without date.
    s_fixes = 0;
    nmea_merge_init(&m, on_fix, NULL);
    add(&m, GGA_1200, 5000);
    nmea_merge_poll(&m, 5000 + NMEA_PAIR_MS - 1);
    CHECK_EQ(s_fixes, 0);
    nmea_merge_poll(&m, 5000 + NMEA_PAIR_MS);
    CHECK_EQ(s_fixes, 1);
    CHECK_EQ(s_fix[0].fix_time_ms, 0);
    CHECK(!s_fix[0].has_speed_mps);

    // Two GGAs in a row: the first goes out alone, the second waits.
    s_fixes = 0;
    add(&m, GGA_1200, 6000);
    add(&m, GGA_1201, 7000);
    CHECK_EQ(s_fixes, 1);
    CHECK_EQ(s_fix[0].board_time_ms, 6000);
    // An RMC of another second sends the waiting GGA alone and is kept.
    add(&m, RMC_1200, 7001);
    CHECK_EQ(s_fixes, 2);
    CHECK_EQ(s_fix[1].fix_time_ms, 0);
    add(&m, GGA_1200, 7002);  // matches the kept RMC (same time of day)
    CHECK_EQ(s_fixes, 3);
    CHECK(s_fix[2].fix_time_ms != 0);

    // No fix: GGA with quality 0 or without a position sends nothing.
    s_fixes = 0;
    nmea_merge_init(&m, on_fix, NULL);
    add(&m, "GPRMC,120000.00,V,,,,,,,221026,,,N", 100);
    add(&m, "GPGGA,120000.00,,,,,0,00,99.99,,,,,,", 101);
    add(&m, "GPGGA,120001.00,4807.038,N,01131.000,E,0,00,99.99,,,,,,", 1200);
    nmea_merge_poll(&m, 5000);
    CHECK_EQ(s_fixes, 0);

    // RMC with status V: paired, but its date, speed and course are not used.
    s_fixes = 0;
    nmea_merge_init(&m, on_fix, NULL);
    add(&m, "GPRMC,120000.00,V,4807.038,N,01131.000,E,1.0,90.0,221026,,,N", 10);
    add(&m, GGA_1200, 11);
    CHECK_EQ(s_fixes, 1);
    CHECK_EQ(s_fix[0].fix_time_ms, 0);
    CHECK(!s_fix[0].has_speed_mps && !s_fix[0].has_course_deg);

    // A placeholder date (before NMEA_MIN_YEAR) gives fix_time_ms 0.
    s_fixes = 0;
    add(&m, "GPRMC,120001.00,A,4807.038,N,01131.000,E,1.0,90.0,060180,,,A", 20);
    add(&m, GGA_1201, 21);
    CHECK_EQ(s_fixes, 1);
    CHECK_EQ(s_fix[0].fix_time_ms, 0);
    CHECK(s_fix[0].has_speed_mps);

    // Quality 7 (manual) and 8 (simulation) are not fixes; 6 (estimated) is.
    s_fixes = 0;
    nmea_merge_init(&m, on_fix, NULL);
    add(&m, "GPGGA,120000.00,4807.038,N,01131.000,E,7,08,0.9,545.4,M,46.9,M,,", 30);
    add(&m, "GPGGA,120001.00,4807.038,N,01131.000,E,8,08,0.9,545.4,M,46.9,M,,", 31);
    add(&m, "GPGGA,120002.00,4807.038,N,01131.000,E,6,08,0.9,545.4,M,46.9,M,,", 32);
    nmea_merge_poll(&m, 5000);
    CHECK_EQ(s_fixes, 1);
    CHECK_EQ(s_fix[0].fix_quality, 6);

    // The largest fix encodes within the record budget at MTU 185.
    ns_board_v1_Record rec = ns_board_v1_Record_init_zero;
    rec.which_kind = ns_board_v1_Record_gps_tag;
    rec.kind.gps = s_fix[0];
    nmea_merge_init(&m, on_fix, NULL);
    add(&m, RMC_1200, 1);
    add(&m, GGA_1200, 2);
    rec.kind.gps = s_fix[0];
    rec.kind.gps.board_time_ms = 4102444800000ull;
    rec.kind.gps.latitude = -89.999999;
    rec.kind.gps.longitude = -179.999999;
    size_t size = ns_proto_record_size(&rec);
    CHECK(size > 0 && size <= ns_proto_max_record_size(185));
    printf("nmea: largest GpsFix record %zu bytes\n", size);
}

// ---------------------------------------------------------------------------
// Garbage

static uint32_t s_rng = 0x13579BDFu;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void test_fuzz(void)
{
    const char *bodies[] = {GGA_1200, RMC_1200, "GNGGA,000001,3352.1234,S,15112.5000,W,1,05,1.5,-12.5,M,,M,,"};
    nmea_line_t l;
    nmea_line_init(&l);
    nmea_merge_t m;
    nmea_merge_init(&m, on_fix, NULL);
    int parsed = 0, lines = 0;
    for (int iter = 0; iter < 100000; iter++) {
        char buf[300];
        if (iter % 3 == 0) {
            size_t n = rnd() % 200;
            for (size_t i = 0; i < n; i++) {
                buf[i] = (char)(1 + rnd() % 255);
            }
            buf[n] = '\0';
        } else {
            // Mutate a valid body, then add a correct checksum about half the time.
            char body[200];
            strcpy(body, bodies[rnd() % 3]);
            size_t len = strlen(body);
            int mutations = 1 + (int)(rnd() % 3);
            for (int k = 0; k < mutations; k++) {
                const char alphabet[] = "0123456789.,-NSEWAVM*$ ";
                body[rnd() % len] = alphabet[rnd() % (sizeof(alphabet) - 1)];
            }
            if (rnd() % 2) {
                strcpy(buf, sentence(body));
            } else {
                snprintf(buf, sizeof(buf), "$%s*00", body);
            }
            strcat(buf, "\r\n");
        }
        for (char *p = buf; *p; p++) {
            if (nmea_line_feed(&l, (uint8_t)*p)) {
                lines++;
                nmea_sentence_t s;
                nmea_type_t t = nmea_parse(l.buf, &s);
                if (t == NMEA_GGA || t == NMEA_RMC) {
                    parsed++;
                    if (s.has_pos) {
                        CHECK(s.lat >= -90 && s.lat <= 90 && s.lon >= -180 && s.lon <= 180);
                    }
                    if (s.has_time) {
                        CHECK(s.time_ms < 86401000u);
                    }
                    s_fixes = 0;
                    nmea_merge_add(&m, &s, (uint64_t)iter);
                }
            }
        }
    }
    printf("nmea: fuzz %d lines, %d parsed as GGA/RMC\n", lines, parsed);
    CHECK(parsed > 10000);
}

int main(void)
{
    test_lines();
    test_checksum();
    test_gga();
    test_rmc();
    test_epoch();
    test_merge();
    test_fuzz();
    if (s_failures != 0) {
        printf("nmea: %d of %d checks FAILED\n", s_failures, s_checks);
        return 1;
    }
    printf("nmea: OK (%d checks)\n", s_checks);
    return 0;
}
