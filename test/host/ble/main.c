// Host tests for the BLE sniffer's pure C parts: AD structure parsing and
// cutting (ble_ad) and the per-window advertiser table (ble_agg). Payloads
// are synthetic; addresses use the documentation range 00:00:5E:00:53:xx and
// made-up random static addresses (C2:00:5E:..).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_ad.h"
#include "ble_agg.h"
#include "ns_proto_fit.h"
#include "pb_decode.h"
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

#define PUBLIC ns_board_v1_BleAddressType_BLE_ADDRESS_TYPE_PUBLIC
#define RANDOM ns_board_v1_BleAddressType_BLE_ADDRESS_TYPE_RANDOM
#define ADV_IND ns_board_v1_BleAdvType_BLE_ADV_TYPE_ADV_IND
#define NONCONN ns_board_v1_BleAdvType_BLE_ADV_TYPE_ADV_NONCONN_IND
#define EXTENDED ns_board_v1_BleAdvType_BLE_ADV_TYPE_EXTENDED

static const uint8_t ADDR_A[6] = {0x00, 0x00, 0x5E, 0x00, 0x53, 0x01};
static const uint8_t ADDR_B[6] = {0xC2, 0x00, 0x5E, 0x00, 0x00, 0x02};

// AD structure builder.
typedef struct {
    uint8_t buf[256];
    size_t len;
} ad_t;

static void ad_add(ad_t *a, uint8_t type, const void *data, size_t n)
{
    a->buf[a->len++] = (uint8_t)(n + 1);
    a->buf[a->len++] = type;
    memcpy(a->buf + a->len, data, n);
    a->len += n;
}

static void ad_flags(ad_t *a) { ad_add(a, BLE_AD_FLAGS, "\x06", 1); }
static void ad_str(ad_t *a, uint8_t type, const char *s) { ad_add(a, type, s, strlen(s)); }

// ---------------------------------------------------------------------------
// ble_ad

static void test_ad_basic(void)
{
    ad_t a = {0};
    ad_flags(&a);
    ad_str(&a, BLE_AD_NAME_COMPLETE, "ExampleTag");
    ad_add(&a, BLE_AD_TX_POWER, "\xF4", 1);                      // -12 dBm
    ad_add(&a, BLE_AD_MANUFACTURER, "\xFF\xFF\x01\x02\x03", 5);  // test company ID
    ble_ad_info_t info;
    ble_ad_parse(a.buf, a.len, &info);
    CHECK(info.has_flags);
    CHECK_EQ(info.flags, 0x06);
    CHECK(info.name_complete);
    CHECK_EQ(info.name_len, 10);
    CHECK(info.name != NULL && memcmp(info.name, "ExampleTag", 10) == 0);
    CHECK(info.has_tx_power);
    CHECK_EQ(info.tx_power, -12);
    CHECK(info.has_company);
    CHECK_EQ(info.company, 0xFFFF);
    CHECK_EQ(info.valid_len, a.len);
    CHECK(!info.malformed);
}

static void test_ad_names(void)
{
    // Shortened only.
    ad_t a = {0};
    ad_str(&a, BLE_AD_NAME_SHORT, "Ex");
    ble_ad_info_t info;
    ble_ad_parse(a.buf, a.len, &info);
    CHECK(!info.name_complete);
    CHECK_EQ(info.name_len, 2);

    // Complete wins over shortened, in either order.
    ad_t b = {0};
    ad_str(&b, BLE_AD_NAME_SHORT, "Ex");
    ad_str(&b, BLE_AD_NAME_COMPLETE, "ExampleNet");
    ble_ad_parse(b.buf, b.len, &info);
    CHECK(info.name_complete);
    CHECK_EQ(info.name_len, 10);
    ad_t c = {0};
    ad_str(&c, BLE_AD_NAME_COMPLETE, "ExampleNet");
    ad_str(&c, BLE_AD_NAME_SHORT, "Ex");
    ble_ad_parse(c.buf, c.len, &info);
    CHECK(info.name_complete);
    CHECK_EQ(info.name_len, 10);

    // An empty name is present but has length 0.
    ad_t d = {0};
    ad_add(&d, BLE_AD_NAME_COMPLETE, "", 0);
    ble_ad_parse(d.buf, d.len, &info);
    CHECK(info.name != NULL);
    CHECK_EQ(info.name_len, 0);
}

static void test_ad_edge(void)
{
    ble_ad_info_t info;
    // Empty data.
    ble_ad_parse(NULL, 0, &info);
    CHECK_EQ(info.valid_len, 0);
    CHECK(!info.malformed);
    CHECK(info.name == NULL);

    // Manufacturer data too short for a company ID; the second one counts.
    ad_t a = {0};
    ad_add(&a, BLE_AD_MANUFACTURER, "\x4C", 1);
    ad_add(&a, BLE_AD_MANUFACTURER, "\x34\x12", 2);
    ad_add(&a, BLE_AD_MANUFACTURER, "\x78\x56", 2);
    ble_ad_parse(a.buf, a.len, &info);
    CHECK(info.has_company);
    CHECK_EQ(info.company, 0x1234);

    // Zero length ends the data; padding after it is ignored.
    ad_t p = {0};
    ad_flags(&p);
    p.buf[p.len++] = 0;
    p.buf[p.len++] = 0x05;  // would be a structure, but it is padding
    p.buf[p.len++] = BLE_AD_NAME_COMPLETE;
    ble_ad_parse(p.buf, p.len, &info);
    CHECK_EQ(info.valid_len, 3);
    CHECK(info.name == NULL);
    CHECK(!info.malformed);

    // A structure running past the end is malformed; what came before stays.
    ad_t m = {0};
    ad_flags(&m);
    ad_str(&m, BLE_AD_NAME_COMPLETE, "ExampleNet");
    ble_ad_parse(m.buf, m.len - 1, &info);
    CHECK(info.malformed);
    CHECK_EQ(info.valid_len, 3);
    CHECK(info.name == NULL);
    CHECK(info.has_flags);

    // Type byte only (length 1): no data, not malformed.
    ad_t t = {0};
    t.buf[t.len++] = 1;
    t.buf[t.len++] = BLE_AD_TX_POWER;
    ble_ad_parse(t.buf, t.len, &info);
    CHECK(!info.has_tx_power);
    CHECK(!info.malformed);
    CHECK_EQ(info.valid_len, 2);
}

static void test_ad_cut(void)
{
    ad_t a = {0};
    ad_flags(&a);                                   // 3 bytes: 0..2
    ad_str(&a, BLE_AD_NAME_COMPLETE, "ExampleNet");  // 12 bytes: 3..14
    uint8_t uuid[16] = {0};
    ad_add(&a, 0x07, uuid, sizeof(uuid));           // 18 bytes: 15..32
    CHECK_EQ(a.len, 33);
    CHECK_EQ(ble_ad_cut(a.buf, a.len, 31), 15);     // the UUID would cross 31
    CHECK_EQ(ble_ad_cut(a.buf, a.len, 33), 33);
    CHECK_EQ(ble_ad_cut(a.buf, a.len, 100), 33);
    CHECK_EQ(ble_ad_cut(a.buf, a.len, 15), 15);
    CHECK_EQ(ble_ad_cut(a.buf, a.len, 14), 3);
    CHECK_EQ(ble_ad_cut(a.buf, a.len, 2), 0);
    CHECK_EQ(ble_ad_cut(a.buf, a.len, 0), 0);
    CHECK_EQ(ble_ad_cut(a.buf, 20, 31), 15);        // malformed tail stops the cut
    // Exactly 31 bytes of structures fit whole.
    ad_t b = {0};
    ad_flags(&b);
    uint8_t mfg[26] = {0xFF, 0xFF};
    ad_add(&b, BLE_AD_MANUFACTURER, mfg, sizeof(mfg));
    CHECK_EQ(b.len, 31);
    CHECK_EQ(ble_ad_cut(b.buf, b.len, 31), 31);
}

static uint32_t s_rng = 0x2468ACE1u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

// Random and mutated payloads: no out-of-bounds read (ASan), and every result
// stays inside the data.
static void test_ad_fuzz(void)
{
    ad_t base = {0};
    ad_flags(&base);
    ad_str(&base, BLE_AD_NAME_COMPLETE, "Cafe-Guest sensor");
    ad_add(&base, BLE_AD_MANUFACTURER, "\xFF\xFF\x10\x20", 4);
    ad_add(&base, BLE_AD_TX_POWER, "\x04", 1);
    int with_name = 0;
    for (int iter = 0; iter < 200000; iter++) {
        uint8_t buf[256];
        size_t len;
        if (iter % 2 == 0) {
            len = rnd() % 256;
            for (size_t i = 0; i < len; i++) {
                buf[i] = (uint8_t)(i % 7 == 0 ? rnd() % 32 : rnd());
            }
        } else {
            len = base.len;
            memcpy(buf, base.buf, len);
            int mutations = 1 + (int)(rnd() % 4);
            for (int m = 0; m < mutations; m++) {
                buf[rnd() % len] = (uint8_t)rnd();
            }
            len = rnd() % (len + 1);
        }
        // Copy to an exact-size heap buffer so ASan catches any overread.
        uint8_t *data = len ? malloc(len) : NULL;
        if (len) {
            memcpy(data, buf, len);
        }
        ble_ad_info_t info;
        ble_ad_parse(data, len, &info);
        CHECK(info.valid_len <= len);
        if (info.name != NULL) {
            with_name++;
            CHECK(info.name >= data && info.name + info.name_len <= data + info.valid_len);
        }
        size_t limit = rnd() % 64;
        size_t cut = ble_ad_cut(data, len, limit);
        CHECK(cut <= limit && cut <= info.valid_len);
        // The cut lands on a structure boundary: parsing the prefix is clean.
        ble_ad_info_t pre;
        ble_ad_parse(data, cut, &pre);
        CHECK(!pre.malformed && pre.valid_len == cut);
        free(data);
    }
    printf("ble: fuzz %d of 200000 payloads had a name\n", with_name);
    CHECK(with_name > 20000);
}

// ---------------------------------------------------------------------------
// ble_agg

#define MAX_OUT 64
static ns_board_v1_Record s_out[MAX_OUT];
static int s_out_count;

static void on_record(const ns_board_v1_Record *rec, void *ctx)
{
    (void)ctx;
    if (s_out_count < MAX_OUT) {
        s_out[s_out_count++] = *rec;
    }
}

static ble_agg_entry_t s_entries[4];
static ble_agg_t s_agg;

static void reset(void)
{
    ble_agg_init(&s_agg, s_entries, 4, on_record, NULL);
    s_out_count = 0;
}

static ble_agg_report_t report(const uint8_t *addr, uint8_t type, int8_t rssi, const ad_t *ad, uint64_t t)
{
    ble_agg_report_t r = {.addr = addr, .addr_type = type, .adv_type = ADV_IND, .rssi = rssi,
                          .tx_power = BLE_AGG_NO_VALUE, .data_status = BLE_AGG_DATA_COMPLETE,
                          .data = ad->buf, .len = ad->len, .time_ms = t};
    return r;
}

static const ns_board_v1_BleObservation *find_out(const uint8_t *addr, uint8_t type)
{
    for (int i = 0; i < s_out_count; i++) {
        const ns_board_v1_BleObservation *o = &s_out[i].kind.ble;
        if (s_out[i].which_kind == ns_board_v1_Record_ble_tag && o->address_type == type &&
            memcmp(o->address, addr, 6) == 0) {
            return o;
        }
    }
    return NULL;
}

static void test_agg_window(void)
{
    reset();
    ad_t a1 = {0};
    ad_flags(&a1);
    ad_str(&a1, BLE_AD_NAME_COMPLETE, "ExampleTag");
    ad_add(&a1, BLE_AD_MANUFACTURER, "\xFF\xFF\x01", 3);
    ad_t a2 = {0};  // the same device's other advertisement: no name
    ad_flags(&a2);
    ad_add(&a2, BLE_AD_TX_POWER, "\xF8", 1);

    ble_agg_report_t r = report(ADDR_A, PUBLIC, -70, &a1, 1000);
    CHECK(ble_agg_add(&s_agg, &r));
    r = report(ADDR_A, PUBLIC, -55, &a2, 1100);
    r.adv_type = NONCONN;
    CHECK(ble_agg_add(&s_agg, &r));
    r = report(ADDR_A, PUBLIC, -80, &a2, 1200);
    CHECK(ble_agg_add(&s_agg, &r));
    // Same bytes, other address type: a different advertiser.
    r = report(ADDR_A, RANDOM, -60, &a1, 1300);
    CHECK(ble_agg_add(&s_agg, &r));
    // RSSI not available: counted, RSSI untouched.
    r = report(ADDR_A, PUBLIC, BLE_AGG_NO_VALUE, &a2, 1250);
    CHECK(ble_agg_add(&s_agg, &r));
    CHECK_EQ(s_agg.reports, 5);

    CHECK_EQ(ble_agg_flush(&s_agg), 2);
    CHECK_EQ(s_out_count, 2);
    const ns_board_v1_BleObservation *o = find_out(ADDR_A, PUBLIC);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK_EQ(o->count, 4);
        CHECK_EQ(o->rssi_max, -55);
        CHECK_EQ(o->rssi_last, -80);
        CHECK_EQ(o->board_time_ms, 1250);
        CHECK_EQ(o->adv_type, ADV_IND);  // the last report's type
        CHECK_EQ(o->adv_data.size, a2.len);  // the last advertisement's data
        CHECK(memcmp(o->adv_data.bytes, a2.buf, a2.len) == 0);
        CHECK(!o->adv_data_truncated);
        CHECK_EQ(o->local_name.size, 10);  // kept from the first advertisement
        CHECK(memcmp(o->local_name.bytes, "ExampleTag", 10) == 0);
        CHECK(o->has_company_id);
        CHECK_EQ(o->company_id, 0xFFFF);
        CHECK(o->has_tx_power);
        CHECK_EQ(o->tx_power, -8);
        CHECK_EQ(o->channel, 0);
    }
    o = find_out(ADDR_A, RANDOM);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK_EQ(o->count, 1);
        CHECK(!o->has_tx_power);
    }
    // The table is empty after a flush.
    CHECK_EQ(ble_agg_flush(&s_agg), 0);
    CHECK_EQ(s_out_count, 2);
}

static void test_agg_full(void)
{
    reset();
    ad_t a = {0};
    ad_flags(&a);
    uint8_t addr[6] = {0xC2, 0x00, 0x5E, 0x00, 0x00, 0x00};
    for (int i = 0; i < 4; i++) {
        addr[5] = (uint8_t)i;
        ble_agg_report_t r = report(addr, RANDOM, -60, &a, 10);
        CHECK(ble_agg_add(&s_agg, &r));
    }
    addr[5] = 4;
    ble_agg_report_t r = report(addr, RANDOM, -60, &a, 20);
    CHECK(!ble_agg_add(&s_agg, &r));  // full: nothing added
    addr[5] = 0;
    r = report(addr, RANDOM, -50, &a, 20);
    CHECK(ble_agg_add(&s_agg, &r));   // known addresses still update
    CHECK_EQ(ble_agg_flush(&s_agg), 4);
    addr[5] = 4;
    r = report(addr, RANDOM, -60, &a, 30);
    CHECK(ble_agg_add(&s_agg, &r));   // room again after the flush
    CHECK_EQ(ble_agg_flush(&s_agg), 1);
    CHECK_EQ(s_out_count, 5);
}

static void test_agg_extended(void)
{
    reset();
    // 60 bytes of AD structures: flags, a 16-byte name, a 37-byte service data.
    ad_t a = {0};
    ad_flags(&a);
    ad_str(&a, BLE_AD_NAME_COMPLETE, "ExampleNet-Long1");
    uint8_t svc[37] = {0xAA, 0xFE};
    ad_add(&a, 0x16, svc, sizeof(svc));
    CHECK_EQ(a.len, 60);
    ble_agg_report_t r = report(ADDR_B, RANDOM, -65, &a, 500);
    r.adv_type = EXTENDED;
    r.tx_power = 5;  // extended header TX power, no AD TX power
    CHECK(ble_agg_add(&s_agg, &r));
    ble_agg_flush(&s_agg);
    const ns_board_v1_BleObservation *o = find_out(ADDR_B, RANDOM);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK_EQ(o->adv_type, EXTENDED);
        CHECK_EQ(o->adv_data.size, 21);  // flags + name; the service data would cross 31
        CHECK(o->adv_data_truncated);
        CHECK_EQ(o->local_name.size, 16);
        CHECK(o->has_tx_power);
        CHECK_EQ(o->tx_power, 5);
    }

    // A chained advertisement: the first chunk says more follows. Its
    // continuation updates nothing but the status and is not counted.
    reset();
    ad_t first = {0};
    ad_flags(&first);
    ad_str(&first, BLE_AD_NAME_COMPLETE, "Ex");
    r = report(ADDR_B, RANDOM, -65, &first, 1000);
    r.adv_type = EXTENDED;
    r.data_status = BLE_AGG_DATA_MORE;
    CHECK(ble_agg_add(&s_agg, &r));
    ad_t rest = {0};
    ad_str(&rest, BLE_AD_NAME_SHORT, "ZZ");
    r = report(ADDR_B, RANDOM, -40, &rest, 1010);
    r.adv_type = EXTENDED;
    CHECK(ble_agg_add(&s_agg, &r));
    // A new advertisement after the chain ended counts again.
    r = report(ADDR_B, RANDOM, -70, &first, 1100);
    r.adv_type = EXTENDED;
    CHECK(ble_agg_add(&s_agg, &r));
    ble_agg_flush(&s_agg);
    o = find_out(ADDR_B, RANDOM);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK_EQ(o->count, 2);
        CHECK_EQ(o->rssi_max, -65);  // the continuation's RSSI is not used
        CHECK_EQ(o->rssi_last, -70);
        CHECK(!o->adv_data_truncated);  // the last advertisement was complete
        CHECK_EQ(o->local_name.size, 2);
        CHECK(memcmp(o->local_name.bytes, "Ex", 2) == 0);
    }

    // A "more data" report whose continuation never came: the next report
    // after BLE_AGG_CONTINUE_MS is a new advertisement.
    reset();
    r = report(ADDR_B, RANDOM, -65, &first, 2000);
    r.data_status = BLE_AGG_DATA_MORE;
    CHECK(ble_agg_add(&s_agg, &r));
    r = report(ADDR_B, RANDOM, -60, &first, 2000 + BLE_AGG_CONTINUE_MS + 1);
    CHECK(ble_agg_add(&s_agg, &r));
    ble_agg_flush(&s_agg);
    o = find_out(ADDR_B, RANDOM);
    CHECK(o != NULL && o->count == 2);

    // A malformed trailing structure is cut off and flagged; an empty name
    // does not replace a name seen earlier in the window.
    reset();
    ad_t named = {0};
    ad_str(&named, BLE_AD_NAME_COMPLETE, "ExampleTag");
    r = report(ADDR_A, PUBLIC, -50, &named, 4000);
    CHECK(ble_agg_add(&s_agg, &r));
    ad_t bad = {0};
    ad_flags(&bad);
    ad_add(&bad, BLE_AD_NAME_COMPLETE, "", 0);
    bad.buf[bad.len++] = 9;  // claims 9 more bytes, has 1
    bad.buf[bad.len++] = BLE_AD_MANUFACTURER;
    r = report(ADDR_A, PUBLIC, -50, &bad, 4010);
    CHECK(ble_agg_add(&s_agg, &r));
    ble_agg_flush(&s_agg);
    o = find_out(ADDR_A, PUBLIC);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK(o->adv_data_truncated);
        CHECK_EQ(o->adv_data.size, 5);  // flags + empty name
        CHECK_EQ(o->local_name.size, 10);
    }

    // Truncated by the controller: flagged even though it fits.
    reset();
    r = report(ADDR_B, RANDOM, -65, &first, 3000);
    r.data_status = BLE_AGG_DATA_TRUNCATED;
    CHECK(ble_agg_add(&s_agg, &r));
    ble_agg_flush(&s_agg);
    o = find_out(ADDR_B, RANDOM);
    CHECK(o != NULL && o->adv_data_truncated && o->adv_data.size == first.len);
}

// The largest BleObservation encodes, decodes back and fits a frame at the
// protocol's minimum MTU without trimming.
static void test_agg_encode(void)
{
    reset();
    ad_t a = {0};
    uint8_t name[40];
    memset(name, 'N', sizeof(name));
    ad_add(&a, BLE_AD_NAME_COMPLETE, name, 29);  // 31 bytes of adv data
    ble_agg_report_t r = report(ADDR_B, RANDOM, -128, &a, 0xFFFFFFFFFFFFull);
    CHECK(ble_agg_add(&s_agg, &r));
    s_entries[0].count = 0xFFFF;
    s_entries[0].has_tx_power = true;
    s_entries[0].tx_power = -127;
    s_entries[0].has_company = true;
    s_entries[0].company = 0xFFFF;
    s_entries[0].name_len = BLE_AGG_NAME_MAX;
    memset(s_entries[0].name, 'N', BLE_AGG_NAME_MAX);
    ble_agg_flush(&s_agg);
    CHECK_EQ(s_out_count, 1);
    ns_board_v1_Record rec = s_out[0];
    rec.kind.ble.board_time_ms = 4102444800000ull;  // Unix time, as the link sends it
    uint8_t buf[256];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    CHECK(pb_encode(&os, ns_board_v1_Record_fields, &rec));
    CHECK(os.bytes_written <= ns_proto_max_record_size(185));
    printf("ble: largest BleObservation record %zu bytes (limit at MTU 185: %zu)\n", os.bytes_written,
           ns_proto_max_record_size(185));
    ns_board_v1_Record back = ns_board_v1_Record_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf, os.bytes_written);
    CHECK(pb_decode(&is, ns_board_v1_Record_fields, &back));
    CHECK_EQ(back.which_kind, ns_board_v1_Record_ble_tag);
    CHECK_EQ(back.kind.ble.rssi_max, -128);
    CHECK_EQ(back.kind.ble.tx_power, -127);
    CHECK_EQ(back.kind.ble.adv_data.size, 31);
    CHECK_EQ(back.kind.ble.local_name.size, 31);
    CHECK(memcmp(back.kind.ble.address, ADDR_B, 6) == 0);
}

int main(void)
{
    test_ad_basic();
    test_ad_names();
    test_ad_edge();
    test_ad_cut();
    test_ad_fuzz();
    test_agg_window();
    test_agg_full();
    test_agg_extended();
    test_agg_encode();
    if (s_failures != 0) {
        printf("ble: %d of %d checks FAILED\n", s_failures, s_checks);
        return 1;
    }
    printf("ble: OK (%d checks)\n", s_checks);
    return 0;
}
