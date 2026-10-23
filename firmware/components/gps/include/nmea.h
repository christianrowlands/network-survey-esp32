// NMEA 0183 sentences from a GPS receiver: line framing, checksum, GGA and
// RMC parsing, and pairing GGA with RMC into one GpsFix.
//
// Pure C with no ESP-IDF dependencies (host tested). Any talker ID is
// accepted (GP, GN, GL, GA, GB, BD and so on). Sentences without a valid
// "*hh" checksum are rejected.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ns_board.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

// NMEA allows 82 characters including "$" and CR LF; some receivers send a
// little more.
#define NMEA_LINE_MAX 120

typedef struct {
    char buf[NMEA_LINE_MAX + 1];
    uint16_t len;
    bool active;  // Inside a sentence (after "$")
} nmea_line_t;

void nmea_line_init(nmea_line_t *l);

// Feeds one received byte. Returns true when a sentence ended (CR or LF): it
// is then in l->buf, NUL terminated, starting with "$". Bytes outside a
// sentence, control characters and over-long lines discard the sentence.
bool nmea_line_feed(nmea_line_t *l, uint8_t c);

typedef enum {
    NMEA_INVALID,  // Not a sentence, or the checksum is missing or wrong
    NMEA_OTHER,    // Valid sentence of another type
    NMEA_GGA,
    NMEA_RMC,
} nmea_type_t;

typedef struct {
    nmea_type_t type;
    bool has_time;
    uint32_t time_ms;   // UTC time of day
    bool has_pos;
    double lat;         // Degrees, south negative
    double lon;         // Degrees, west negative
    // GGA
    uint32_t quality;   // 0 invalid, 1 GPS, 2 DGPS, 4 RTK fixed, 5 RTK float, 6 estimated
    uint32_t satellites;
    bool has_hdop;
    float hdop;
    bool has_alt;
    float alt_m;        // Above mean sea level
    // RMC
    bool active;        // Status A (valid)
    bool has_date;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    bool has_speed;
    float speed_mps;
    bool has_course;
    float course_deg;
} nmea_sentence_t;

// Parses one sentence ("$...*hh", without CR LF). Fills out for GGA and RMC.
nmea_type_t nmea_parse(const char *line, nmea_sentence_t *out);

// Unix epoch milliseconds of a UTC date and time of day.
uint64_t nmea_epoch_ms(uint16_t year, uint8_t month, uint8_t day, uint32_t time_ms);

// Pairs each GGA with the RMC of the same time of day (in either order) and
// hands complete fixes to the callback. A GGA whose RMC does not arrive within
// NMEA_PAIR_MS, or whose RMC has status V, goes out alone (no date, speed or
// course). Only GGA sentences with a position and quality 1 to 6 produce a
// fix. RMC dates before NMEA_MIN_YEAR are treated as unknown (fix_time_ms 0).
#define NMEA_PAIR_MS 1500
#define NMEA_MIN_YEAR 2020

typedef void (*nmea_fix_fn)(const ns_board_v1_GpsFix *fix, void *ctx);

typedef struct {
    bool has_gga;
    bool has_rmc;
    nmea_sentence_t gga;
    nmea_sentence_t rmc;
    uint64_t gga_rx_ms;   // When the waiting GGA arrived
    nmea_fix_fn fn;
    void *ctx;
} nmea_merge_t;

void nmea_merge_init(nmea_merge_t *m, nmea_fix_fn fn, void *ctx);

// Feeds a parsed GGA or RMC received at now_ms (monotonic). Other types are ignored.
void nmea_merge_add(nmea_merge_t *m, const nmea_sentence_t *s, uint64_t now_ms);

// Sends a GGA that waited NMEA_PAIR_MS for its RMC. Call about once a second.
void nmea_merge_poll(nmea_merge_t *m, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
