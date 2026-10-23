#include "nmea.h"

#include <string.h>

#define MAX_FIELDS 24
#define KNOTS_TO_MPS 0.514444f

void nmea_line_init(nmea_line_t *l)
{
    memset(l, 0, sizeof(*l));
}

bool nmea_line_feed(nmea_line_t *l, uint8_t c)
{
    if (c == '$') {
        l->buf[0] = '$';
        l->len = 1;
        l->active = true;
        return false;
    }
    if (!l->active) {
        return false;
    }
    if (c == '\r' || c == '\n') {
        l->buf[l->len] = '\0';
        l->active = false;
        return true;
    }
    if (c < 0x20 || c > 0x7E || l->len >= NMEA_LINE_MAX) {
        l->active = false;  // noise or a line too long: wait for the next "$"
        return false;
    }
    l->buf[l->len++] = (char)c;
    return false;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// "$<body>*hh": checks the XOR of the body. Returns the body length, or -1.
static int check(const char *line)
{
    if (line[0] != '$') {
        return -1;
    }
    uint8_t sum = 0;
    int i = 1;
    for (; line[i] != '\0' && line[i] != '*'; i++) {
        sum ^= (uint8_t)line[i];
    }
    if (line[i] != '*') {
        return -1;
    }
    int hi = hex(line[i + 1]);
    int lo = hi < 0 ? -1 : hex(line[i + 2]);
    if (lo < 0 || line[i + 3] != '\0' || ((hi << 4) | lo) != sum) {
        return -1;
    }
    return i - 1;
}

// Unsigned or signed decimal: digits with at most one point. Empty fails, and
// so do more than 15 digits (keeps every conversion below in range).
static bool decimal(const char *s, double *out)
{
    bool neg = *s == '-';
    if (neg) {
        s++;
    }
    double v = 0, scale = 0;
    int digits = 0;
    for (; *s != '\0'; s++) {
        if (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            scale *= 10;
            digits++;
        } else if (*s == '.' && scale == 0) {
            scale = 1;
        } else {
            return false;
        }
    }
    if (digits == 0 || digits > 15) {
        return false;
    }
    *out = (neg ? -v : v) / (scale > 0 ? scale : 1);
    return true;
}

static bool uint_field(const char *s, uint32_t *out)
{
    double v;
    if (!decimal(s, &v) || v < 0 || v > 1e6 || v != (double)(uint32_t)v) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static int two_digits(const char *s)
{
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') {
        return -1;
    }
    return (s[0] - '0') * 10 + (s[1] - '0');
}

// hhmmss[.sss]
static bool parse_time(const char *s, uint32_t *ms)
{
    if (strlen(s) < 6) {
        return false;
    }
    int h = two_digits(s), m = two_digits(s + 2);
    double sec;
    if (h < 0 || h > 23 || m < 0 || m > 59 || !decimal(s + 4, &sec) || sec < 0 || sec >= 61) {
        return false;
    }
    *ms = (uint32_t)h * 3600000u + (uint32_t)m * 60000u + (uint32_t)(sec * 1000 + 0.5);
    return true;
}

// ddmmyy: 80 to 99 are 1980 to 1999 (placeholder dates), 00 to 79 are 2000 on.
static bool parse_date(const char *s, nmea_sentence_t *out)
{
    if (strlen(s) != 6) {
        return false;
    }
    int d = two_digits(s), m = two_digits(s + 2), y = two_digits(s + 4);
    if (d < 1 || d > 31 || m < 1 || m > 12 || y < 0) {
        return false;
    }
    out->day = (uint8_t)d;
    out->month = (uint8_t)m;
    out->year = (uint16_t)(y >= 80 ? 1900 + y : 2000 + y);
    return true;
}

// (d)ddmm.mmmm plus hemisphere letter.
static bool parse_coord(const char *value, const char *hemi, bool is_lat, double *out)
{
    double v;
    if (!decimal(value, &v) || v < 0 || v >= 18100 || hemi[0] == '\0' || hemi[1] != '\0') {
        return false;
    }
    double deg = (double)(int)(v / 100);
    double min = v - deg * 100;
    if (min >= 60 || deg > (is_lat ? 90 : 180)) {
        return false;
    }
    v = deg + min / 60;
    char pos = is_lat ? 'N' : 'E', neg = is_lat ? 'S' : 'W';
    if (hemi[0] == neg) {
        v = -v;
    } else if (hemi[0] != pos) {
        return false;
    }
    if (v > (is_lat ? 90 : 180) || v < (is_lat ? -90 : -180)) {
        return false;
    }
    *out = v;
    return true;
}

static void parse_gga(char **f, int n, nmea_sentence_t *out)
{
    out->has_time = n > 1 && parse_time(f[1], &out->time_ms);
    out->has_pos = n > 5 && parse_coord(f[2], f[3], true, &out->lat) && parse_coord(f[4], f[5], false, &out->lon);
    if (n > 6) {
        uint_field(f[6], &out->quality);
    }
    if (n > 7) {
        uint_field(f[7], &out->satellites);
    }
    double v;
    if (n > 8 && decimal(f[8], &v)) {
        out->has_hdop = true;
        out->hdop = (float)v;
    }
    if (n > 10 && decimal(f[9], &v) && f[10][0] == 'M') {
        out->has_alt = true;
        out->alt_m = (float)v;
    }
}

static void parse_rmc(char **f, int n, nmea_sentence_t *out)
{
    out->has_time = n > 1 && parse_time(f[1], &out->time_ms);
    out->active = n > 2 && f[2][0] == 'A';
    out->has_pos = n > 6 && parse_coord(f[3], f[4], true, &out->lat) && parse_coord(f[5], f[6], false, &out->lon);
    double v;
    if (n > 7 && decimal(f[7], &v) && v >= 0) {
        out->has_speed = true;
        out->speed_mps = (float)v * KNOTS_TO_MPS;
    }
    if (n > 8 && decimal(f[8], &v) && v >= 0 && v < 360) {
        out->has_course = true;
        out->course_deg = (float)v;
    }
    out->has_date = n > 9 && parse_date(f[9], out);
}

nmea_type_t nmea_parse(const char *line, nmea_sentence_t *out)
{
    memset(out, 0, sizeof(*out));
    int body = check(line);
    if (body < 5 || body > NMEA_LINE_MAX) {
        return out->type = NMEA_INVALID;
    }
    char buf[NMEA_LINE_MAX + 1];
    memcpy(buf, line + 1, (size_t)body);
    buf[body] = '\0';
    char *fields[MAX_FIELDS];
    int n = 0;
    char *p = buf;
    fields[n++] = p;
    for (; *p != '\0'; p++) {
        if (*p == ',') {
            *p = '\0';
            if (n == MAX_FIELDS) {
                break;
            }
            fields[n++] = p + 1;
        }
    }
    // Address field: two-letter talker and three-letter type (proprietary
    // sentences, "$P...", are valid but not ours).
    const char *addr = fields[0];
    if (strlen(addr) != 5 || addr[0] == 'P') {
        return out->type = NMEA_OTHER;
    }
    if (strcmp(addr + 2, "GGA") == 0) {
        out->type = NMEA_GGA;
        parse_gga(fields, n, out);
    } else if (strcmp(addr + 2, "RMC") == 0) {
        out->type = NMEA_RMC;
        parse_rmc(fields, n, out);
    } else {
        out->type = NMEA_OTHER;
    }
    return out->type;
}

uint64_t nmea_epoch_ms(uint16_t year, uint8_t month, uint8_t day, uint32_t time_ms)
{
    // Days from 1970-01-01 (proleptic Gregorian calendar).
    int y = year - (month <= 2);
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return (uint64_t)days * 86400000u + time_ms;
}

void nmea_merge_init(nmea_merge_t *m, nmea_fix_fn fn, void *ctx)
{
    memset(m, 0, sizeof(*m));
    m->fn = fn;
    m->ctx = ctx;
}

static void emit(nmea_merge_t *m, const nmea_sentence_t *rmc)
{
    const nmea_sentence_t *g = &m->gga;
    m->has_gga = false;
    // Quality 1 to 6 is a fix; 7 (manual) and 8 (simulation) are not.
    if (!g->has_pos || g->quality == 0 || g->quality > 6 || m->fn == NULL) {
        return;
    }
    if (rmc != NULL && !rmc->active) {
        rmc = NULL;  // status V: its date, speed and course may be stale
    }
    ns_board_v1_GpsFix fix = ns_board_v1_GpsFix_init_zero;
    fix.board_time_ms = m->gga_rx_ms;
    fix.latitude = g->lat;
    fix.longitude = g->lon;
    fix.has_altitude_m = g->has_alt;
    fix.altitude_m = g->alt_m;
    fix.has_hdop = g->has_hdop;
    fix.hdop = g->hdop;
    fix.satellites = g->satellites;
    fix.fix_quality = g->quality;
    if (rmc != NULL) {
        // Receivers without a time fix yet report placeholder dates.
        if (rmc->has_date && rmc->year >= NMEA_MIN_YEAR && g->has_time) {
            fix.fix_time_ms = nmea_epoch_ms(rmc->year, rmc->month, rmc->day, g->time_ms);
        }
        fix.has_speed_mps = rmc->has_speed;
        fix.speed_mps = rmc->speed_mps;
        fix.has_course_deg = rmc->has_course;
        fix.course_deg = rmc->course_deg;
    }
    m->fn(&fix, m->ctx);
}

static bool same_time(const nmea_sentence_t *a, const nmea_sentence_t *b)
{
    return a->has_time && b->has_time && a->time_ms == b->time_ms;
}

void nmea_merge_add(nmea_merge_t *m, const nmea_sentence_t *s, uint64_t now_ms)
{
    if (s->type == NMEA_GGA) {
        if (m->has_gga) {
            emit(m, NULL);  // its RMC never came
        }
        m->gga = *s;
        m->gga_rx_ms = now_ms;
        m->has_gga = true;
        if (m->has_rmc && same_time(&m->rmc, s)) {
            m->has_rmc = false;
            emit(m, &m->rmc);
        }
    } else if (s->type == NMEA_RMC) {
        if (m->has_gga && same_time(&m->gga, s)) {
            emit(m, s);
            return;
        }
        if (m->has_gga) {
            emit(m, NULL);
        }
        m->rmc = *s;
        m->has_rmc = true;
    }
}

void nmea_merge_poll(nmea_merge_t *m, uint64_t now_ms)
{
    if (m->has_gga && now_ms - m->gga_rx_ms >= NMEA_PAIR_MS) {
        emit(m, NULL);
    }
}
