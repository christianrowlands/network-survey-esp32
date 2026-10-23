// Host tests for the link layer: notification framing (MTU boundaries,
// RecordBatch building, sequence wrap) and the drop-oldest ring buffer.
//
// The framing fixtures in test/fixtures/framing/ are produced here and are
// meant to be decoded by the iOS BoardFraming rules (see the README there).
// Frames are also decoded in this file with a C port of those rules.
//
// Usage: link_test <fixtures-root> [--write]
//   <fixtures-root> is test/fixtures (reads proto/, writes or checks framing/).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link_frame.h"
#include "link_ring.h"
#include "ns_board.pb.h"
#include "ns_proto_fit.h"
#include "pb_decode.h"
#include "pb_encode.h"

static int failures;
static const char *fixtures_root;
static int write_mode;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------------------
// Frame decoder, ported from BoardFraming.decode in the iOS BoardProtocol
// package: u16 LE sequence, then varint length + message until the end.

#define MAX_MSGS 32

typedef struct {
    uint16_t seq;
    int count;
    const uint8_t *msg[MAX_MSGS];
    size_t len[MAX_MSGS];
} decoded_t;

static bool decode_frame(const uint8_t *p, size_t n, decoded_t *out)
{
    if (n < 2) {
        return false;
    }
    out->seq = (uint16_t)(p[0] | (p[1] << 8));
    out->count = 0;
    size_t off = 2;
    while (off < n) {
        uint64_t len = 0;
        unsigned shift = 0;
        for (;;) {
            if (off >= n || shift >= 35) {
                return false;
            }
            uint8_t b = p[off++];
            len |= (uint64_t)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                break;
            }
            shift += 7;
        }
        if (len > n - off || out->count >= MAX_MSGS) {
            return false;
        }
        out->msg[out->count] = p + off;
        out->len[out->count] = (size_t)len;
        out->count++;
        off += (size_t)len;
    }
    return true;
}

// Phone-side gap rule from protocol.md section 4.
static unsigned seq_gap(uint16_t prev, uint16_t next)
{
    uint16_t gap = (uint16_t)(next - prev - 1);
    return gap >= 0x8000 ? 0 : gap;
}

// ---------------------------------------------------------------------------
// Fixture helpers.

static size_t read_file(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

// Encoded Record from a single-record RecordBatch fixture in fixtures/proto.
static size_t load_record(const char *name, uint8_t *out, size_t cap, ns_board_v1_Record *rec)
{
    char path[512];
    uint8_t raw[1024];
    snprintf(path, sizeof(path), "%s/proto/%s.bin", fixtures_root, name);
    size_t n = read_file(path, raw, sizeof(raw));
    static ns_board_v1_RecordBatch batch;
    pb_istream_t is = pb_istream_from_buffer(raw, n);
    if (n == 0 || !pb_decode(&is, ns_board_v1_RecordBatch_fields, &batch) || batch.records_count != 1) {
        fprintf(stderr, "cannot load record fixture %s\n", path);
        failures++;
        return 0;
    }
    *rec = batch.records[0];
    pb_ostream_t os = pb_ostream_from_buffer(out, cap);
    CHECK(pb_encode(&os, ns_board_v1_Record_fields, rec));
    return os.bytes_written;
}

static size_t encode(const pb_msgdesc_t *fields, const void *msg, uint8_t *out, size_t cap)
{
    pb_ostream_t os = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&os, fields, msg)) {
        fprintf(stderr, "encode failed: %s\n", PB_GET_ERROR(&os));
        failures++;
        return 0;
    }
    return os.bytes_written;
}

// Writes or compares test/fixtures/framing/<name>.bin.
static void fixture(const char *name, const uint8_t *frame, size_t len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/framing/%s.bin", fixtures_root, name);
    if (write_mode) {
        FILE *f = fopen(path, "wb");
        if (f == NULL || fwrite(frame, 1, len, f) != len) {
            fprintf(stderr, "cannot write %s\n", path);
            failures++;
        }
        if (f != NULL) {
            fclose(f);
        }
    } else {
        uint8_t want[LINK_FRAME_MAX + 1];
        size_t n = read_file(path, want, sizeof(want));
        if (n != len || memcmp(want, frame, len) != 0) {
            fprintf(stderr, "%s: fixture missing or different (run make fixtures)\n", path);
            failures++;
        }
    }
    printf("  %-34s %3zu bytes\n", name, len);
}

// ---------------------------------------------------------------------------

static void test_varint(void)
{
    static const struct {
        uint32_t v;
        size_t n;
    } cases[] = {{0, 1}, {127, 1}, {128, 2}, {16383, 2}, {16384, 3}, {UINT32_MAX, 5}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t buf[5];
        CHECK(link_varint_size(cases[i].v) == cases[i].n);
        CHECK(link_varint_write(buf, cases[i].v) == cases[i].n);
        pb_istream_t is = pb_istream_from_buffer(buf, cases[i].n);
        uint32_t back = 0;
        CHECK(pb_decode_varint32(&is, &back) && back == cases[i].v);
    }
}

static void test_payload_sizes(void)
{
    CHECK(link_frame_payload(185) == 182);
    CHECK(link_frame_payload(517) == 514);
    CHECK(link_frame_payload(527) == LINK_FRAME_MAX);
    CHECK(link_frame_payload(3) == 0);
    // Same values as BoardFraming.maxMessageSize on iOS.
    CHECK(link_frame_max_message(182) == 178);
    CHECK(link_frame_max_message(514) == 510);
    CHECK(link_frame_max_message(130) == 127);
    CHECK(link_frame_max_message(131) == 127);
    CHECK(link_frame_max_message(132) == 128);
    CHECK(link_frame_max_message(3) == 0);
    // A Record alone in a Records frame: never less than ns_proto_fit's budget.
    for (uint16_t mtu = 185; mtu <= 517; mtu++) {
        size_t payload = link_frame_payload(mtu);
        size_t max = link_batch_max_record(payload);
        CHECK(max >= ns_proto_max_record_size(mtu));
        uint8_t buf[LINK_FRAME_MAX];
        static uint8_t rec[LINK_FRAME_MAX];
        memset(rec, 0x55, sizeof(rec));
        link_batch_t b;
        link_batch_init(&b, buf, payload, 0, true);
        CHECK(link_batch_can_add(&b, max) && !link_batch_can_add(&b, max + 1));
        CHECK(link_batch_add(&b, rec, max));
        CHECK(link_batch_finish(&b) <= payload);
        link_batch_init(&b, buf, payload, 0, true);
        CHECK(!link_batch_add(&b, rec, max + 1));
    }
}

// Generic messages at the MTU boundary: exact fit, one byte over, several.
static void test_frame_boundaries(void)
{
    static uint8_t msg[LINK_FRAME_MAX];
    for (size_t i = 0; i < sizeof(msg); i++) {
        msg[i] = (uint8_t)i;
    }
    uint8_t buf[LINK_FRAME_MAX];
    link_frame_t f;
    decoded_t d;

    const size_t payloads[] = {182, 244, 514, 130, 131, 132};
    for (size_t k = 0; k < sizeof(payloads) / sizeof(payloads[0]); k++) {
        size_t cap = payloads[k];
        size_t max = link_frame_max_message(cap);
        link_frame_init(&f, buf, cap, 1);
        CHECK(!link_frame_add(&f, msg, max + 1) && f.len == 2 && f.count == 0);
        CHECK(link_frame_add(&f, msg, max));
        CHECK(f.len <= cap && cap - f.len <= 1);  // exact, or 1 short at a varint step
        CHECK(decode_frame(buf, f.len, &d) && d.seq == 1 && d.len[0] == max &&
              memcmp(d.msg[0], msg, max) == 0);
    }

    // Two messages that fill a 182-byte frame exactly: 2 + (1+89) + (1+89).
    link_frame_init(&f, buf, 182, 0xBEEF);
    CHECK(link_frame_add(&f, msg, 89));
    CHECK(link_frame_add(&f, msg + 89, 89));
    CHECK(f.len == 182);
    CHECK(decode_frame(buf, f.len, &d) && d.seq == 0xBEEF && d.count == 2 && d.len[1] == 89 &&
          memcmp(d.msg[1], msg + 89, 89) == 0);

    // A message that would cross the end goes into the next frame unchanged.
    link_frame_init(&f, buf, 182, 0);
    CHECK(link_frame_add(&f, msg, 100));
    CHECK(!link_frame_add(&f, msg, 80));  // 2 + 101 + 81 = 184
    CHECK(link_frame_add(&f, msg, 78));   // 2 + 101 + 79 = 182
    CHECK(f.len == 182 && f.count == 2);
}

// RecordBatch frames: records decode back with nanopb, the batch length
// varint switches between 1 and 2 bytes correctly, at most 12 records.
static void test_batch(void)
{
    static uint8_t rec[LINK_FRAME_MAX];
    ns_board_v1_Record r;
    size_t n = load_record("record_client", rec, sizeof(rec), &r);
    CHECK(n > 0 && n < 60);

    uint8_t buf[LINK_FRAME_MAX];
    link_batch_t b;
    decoded_t d;
    static ns_board_v1_RecordBatch batch;

    // Small batch: one-byte length, unsynced (no time_synced field).
    link_batch_init(&b, buf, 182, 3, false);
    CHECK(link_batch_add(&b, rec, n));
    size_t len = link_batch_finish(&b);
    CHECK(len == 2 + 1 + 1 + 1 + n);
    CHECK(decode_frame(buf, len, &d) && d.count == 1 && d.seq == 3);
    pb_istream_t is = pb_istream_from_buffer(d.msg[0], d.len[0]);
    CHECK(pb_decode(&is, ns_board_v1_RecordBatch_fields, &batch));
    CHECK(!batch.time_synced && batch.records_count == 1 &&
          batch.records[0].which_kind == ns_board_v1_Record_client_tag);

    // Many small records: two-byte length, at most 12 records.
    static uint8_t tiny[16];
    ns_board_v1_Record t = ns_board_v1_Record_init_zero;
    t.which_kind = ns_board_v1_Record_gps_tag;
    t.kind.gps.satellites = 4;
    t.kind.gps.latitude = 1.5;
    size_t tn = encode(ns_board_v1_Record_fields, &t, tiny, sizeof(tiny));
    CHECK(tn > 0 && link_batch_record_cost(tn) * 13 < 510);
    link_batch_init(&b, buf, 514, 4, true);
    int added = 0;
    while (link_batch_add(&b, tiny, tn)) {
        added++;
    }
    CHECK(added == LINK_BATCH_MAX_RECORDS && !link_batch_can_add(&b, tn));
    len = link_batch_finish(&b);
    CHECK(len <= 514);
    CHECK(decode_frame(buf, len, &d) && d.count == 1);
    is = pb_istream_from_buffer(d.msg[0], d.len[0]);
    CHECK(pb_decode(&is, ns_board_v1_RecordBatch_fields, &batch));
    CHECK(batch.time_synced && batch.records_count == LINK_BATCH_MAX_RECORDS);

    // Empty batch yields no frame.
    link_batch_init(&b, buf, 182, 5, true);
    CHECK(link_batch_finish(&b) == 0);

    // Byte budget, not count, ends a batch at MTU 185.
    link_batch_init(&b, buf, 182, 6, true);
    added = 0;
    while (link_batch_add(&b, rec, n)) {
        added++;
    }
    CHECK(added == (int)((182 - 2 - 2 - 2) / link_batch_record_cost(n)));
    len = link_batch_finish(&b);
    CHECK(len <= 182 && 182 - len < link_batch_record_cost(n));
    CHECK(decode_frame(buf, len, &d) && d.count == 1);
}

static void test_seq_wrap(void)
{
    link_seq_t s = {.next = 65534};
    CHECK(link_seq_take(&s) == 65534);
    CHECK(link_seq_take(&s) == 65535);
    CHECK(link_seq_take(&s) == 0);
    CHECK(link_seq_take(&s) == 1);
    link_seq_reset(&s);
    CHECK(link_seq_take(&s) == 0);

    CHECK(seq_gap(65535, 0) == 0);
    CHECK(seq_gap(65534, 1) == 2);
    CHECK(seq_gap(10, 3) == 0);    // restart after resubscribe
    CHECK(seq_gap(5, 5) == 0);     // duplicate
    CHECK(seq_gap(0, 32768) == 32767);
}

// ---------------------------------------------------------------------------
// Ring buffer.

static void fill_pattern(uint8_t *p, size_t n, uint32_t id)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(id * 31 + i);
    }
}

static void test_ring_basic(void)
{
    uint8_t mem[64];
    link_ring_t r;
    link_ring_init(&r, mem, sizeof(mem));
    uint8_t in[32], out[32], tag = 0;
    size_t len = 0;

    CHECK(!link_ring_peek(&r, &tag, &len) && link_ring_pop(&r, out, sizeof(out), &tag) == 0);
    CHECK(link_ring_push(&r, 1, in, 0) == 0 && r.entries == 0);

    fill_pattern(in, 10, 1);
    CHECK(link_ring_push(&r, 7, in, 10) == 0);
    CHECK(r.used == 13 && r.entries == 1 && link_ring_fill_percent(&r) == 21);
    CHECK(link_ring_peek(&r, &tag, &len) && tag == 7 && len == 10);
    CHECK(link_ring_pop(&r, out, 9, &tag) == 0 && r.entries == 1);  // too small: stays
    CHECK(link_ring_pop(&r, out, sizeof(out), &tag) == 10 && memcmp(in, out, 10) == 0);
    CHECK(r.used == 0 && r.entries == 0 && r.dropped == 0);

    // An entry larger than the ring is dropped and counted.
    static uint8_t big[100];
    CHECK(link_ring_push(&r, 0, big, 62) == -1 && r.dropped == 1 && r.entries == 0);
    CHECK(link_ring_push(&r, 0, big, 61) == 0 && r.used == 64 && link_ring_fill_percent(&r) == 100);
    link_ring_clear(&r);
    CHECK(r.used == 0 && r.entries == 0 && r.dropped == 1);
}

// Entries split across the end of the buffer survive intact; overflow evicts
// the oldest entries and counts exactly how many.
static void test_ring_overflow(void)
{
    uint8_t mem[50];
    link_ring_t r;
    link_ring_init(&r, mem, sizeof(mem));
    uint8_t in[20], out[20], tag;

    // 3 entries of 3 + 12 = 15 bytes: 45 used.
    for (uint32_t id = 0; id < 3; id++) {
        fill_pattern(in, 12, id);
        CHECK(link_ring_push(&r, (uint8_t)id, in, 12) == 0);
    }
    CHECK(r.used == 45 && r.entries == 3);
    // A fourth needs 15 more: evicts entry 0 only.
    fill_pattern(in, 12, 3);
    CHECK(link_ring_push(&r, 3, in, 12) == 1);
    CHECK(r.entries == 3 && r.dropped == 1 && r.used == 45);
    // A 17-byte entry (20 with header) needs 15 free + one more eviction.
    fill_pattern(in, 17, 4);
    CHECK(link_ring_push(&r, 4, in, 17) == 1);
    CHECK(r.entries == 3 && r.dropped == 2 && r.used == 50);

    // Remaining order: 2, 3, 4; contents intact although they wrapped.
    uint32_t want_ids[] = {2, 3, 4};
    size_t want_len[] = {12, 12, 17};
    for (int i = 0; i < 3; i++) {
        uint8_t expect[20];
        fill_pattern(expect, want_len[i], want_ids[i]);
        size_t n = link_ring_pop(&r, out, sizeof(out), &tag);
        CHECK(n == want_len[i] && tag == want_ids[i] && memcmp(out, expect, n) == 0);
    }
    CHECK(r.entries == 0 && r.used == 0);
}

// Random pushes and pops against a simple reference model.
static void test_ring_model(void)
{
    enum { CAP = 1000, MODEL = 4096 };
    static uint8_t mem[CAP];
    link_ring_t r;
    link_ring_init(&r, mem, CAP);

    static uint32_t ids[MODEL];
    static size_t lens[MODEL];
    size_t head = 0, tail = 0, used = 0;
    uint32_t dropped = 0, next_id = 0;
    uint32_t rng = 12345;
    uint8_t in[300], out[300], expect[300], tag;

    for (int step = 0; step < 200000; step++) {
        rng = rng * 1103515245u + 12345u;
        bool push = ((rng >> 16) % 100) < 60;
        if (push) {
            size_t len = 1 + (rng >> 8) % 250;
            fill_pattern(in, len, next_id);
            int evicted = 0;
            while (CAP - used < LINK_RING_ENTRY_HEADER + len) {
                used -= LINK_RING_ENTRY_HEADER + lens[tail % MODEL];
                tail++;
                evicted++;
            }
            ids[head % MODEL] = next_id;
            lens[head % MODEL] = len;
            head++;
            used += LINK_RING_ENTRY_HEADER + len;
            dropped += (uint32_t)evicted;
            CHECK(link_ring_push(&r, (uint8_t)next_id, in, len) == evicted);
            next_id++;
        } else {
            size_t n = link_ring_pop(&r, out, sizeof(out), &tag);
            if (tail == head) {
                CHECK(n == 0);
            } else {
                fill_pattern(expect, lens[tail % MODEL], ids[tail % MODEL]);
                CHECK(n == lens[tail % MODEL] && tag == (uint8_t)ids[tail % MODEL] &&
                      memcmp(out, expect, n) == 0);
                used -= LINK_RING_ENTRY_HEADER + n;
                tail++;
            }
        }
        if (r.used != used || r.entries != head - tail || r.dropped != dropped) {
            fprintf(stderr, "ring model diverged at step %d\n", step);
            failures++;
            return;
        }
    }
    printf("  %-34s %u pushed, %u dropped\n", "ring model", next_id, dropped);
}

// ---------------------------------------------------------------------------
// Interop fixtures for the iOS decoder (test/fixtures/framing/README.md).

// Grows an AP record (pairwise ciphers, extended rates, SSID) until it
// encodes to exactly target bytes. Deterministic, so the fixture is stable.
static bool size_ap_exactly(ns_board_v1_Record *r, size_t target)
{
    ns_board_v1_ApObservation *ap = &r->kind.ap;
    for (int ciphers = 1; ciphers <= 8; ciphers++) {
        ap->pairwise_ciphers_count = (pb_size_t)ciphers;
        for (int i = 0; i < ciphers; i++) {
            ap->pairwise_ciphers[i] = 0x000FAC04;
        }
        for (int ext = 0; ext <= 32; ext++) {
            ap->extended_supported_rates.size = (pb_size_t)ext;
            memset(ap->extended_supported_rates.bytes, 0x6C, (size_t)ext);
            for (int ssid = 1; ssid <= 32; ssid++) {
                ap->ssid.size = (pb_size_t)ssid;
                memset(ap->ssid.bytes, 'X', (size_t)ssid);
                memcpy(ap->ssid.bytes, "ExampleNet", ssid < 10 ? (size_t)ssid : 10);
                if (ns_proto_record_size(r) == target) {
                    return true;
                }
            }
        }
    }
    return false;
}

static void make_fixtures(void)
{
    uint8_t buf[LINK_FRAME_MAX];
    uint8_t msg[LINK_FRAME_MAX];
    link_frame_t f;
    decoded_t d;

    // The protocol.md section 14 Ping response, byte for byte.
    ns_board_v1_ControlResponse resp = ns_board_v1_ControlResponse_init_zero;
    resp.request_id = 5;
    resp.result = ns_board_v1_ResultCode_RESULT_CODE_OK;
    resp.which_payload = ns_board_v1_ControlResponse_ping_tag;
    resp.payload.ping.nonce = 1;
    resp.payload.ping.board_time_ms = 1767225600123ULL;
    resp.payload.ping.time_synced = true;
    size_t n = encode(ns_board_v1_ControlResponse_fields, &resp, msg, sizeof(msg));
    link_frame_init(&f, buf, 182, 0);
    CHECK(link_frame_add(&f, msg, n));
    static const uint8_t ping_doc[] = {0x00, 0x00, 0x11, 0x08, 0x05, 0x10, 0x01, 0x3a, 0x0b, 0x08,
                                       0x01, 0x10, 0xfb, 0xd0, 0xea, 0xb6, 0xb7, 0x33, 0x18, 0x01};
    CHECK(f.len == sizeof(ping_doc) && memcmp(buf, ping_doc, f.len) == 0);
    fixture("control_ping_response_seq0", buf, f.len);

    // The protocol.md section 14 OTA ack, sequence 258.
    ns_board_v1_OtaAck ack = {.next_offset = 4096, .status = ns_board_v1_OtaAckStatus_OTA_ACK_STATUS_OK};
    n = encode(ns_board_v1_OtaAck_fields, &ack, msg, sizeof(msg));
    link_frame_init(&f, buf, 182, 258);
    CHECK(link_frame_add(&f, msg, n));
    static const uint8_t ack_doc[] = {0x02, 0x01, 0x05, 0x08, 0x80, 0x20, 0x10, 0x01};
    CHECK(f.len == sizeof(ack_doc) && memcmp(buf, ack_doc, f.len) == 0);
    fixture("ota_ack_seq258", buf, f.len);

    // Status: the board_status fixture message, twice in one frame (sequence 7).
    char path[512];
    snprintf(path, sizeof(path), "%s/proto/board_status.bin", fixtures_root);
    n = read_file(path, msg, sizeof(msg));
    CHECK(n > 0);
    link_frame_init(&f, buf, 182, 7);
    CHECK(link_frame_add(&f, msg, n) && link_frame_add(&f, msg, n));
    fixture("status_two_messages_seq7", buf, f.len);

    // Records at MTU 185 starting at sequence 65535, so the second frame is 0:
    // ap, client, probe, ble, gps, packed in order as the board does.
    static const char *names[] = {"record_ap", "record_client", "record_probe", "record_ble", "record_gps"};
    static uint8_t recs[5][LINK_FRAME_MAX];
    size_t lens[5];
    ns_board_v1_Record r;
    for (int i = 0; i < 5; i++) {
        lens[i] = load_record(names[i], recs[i], sizeof(recs[i]), &r);
    }
    link_seq_t seq = {.next = 65535};
    link_batch_t b;
    int next = 0, frame_no = 0;
    uint16_t prev_seq = 0;
    while (next < 5) {
        uint16_t s = link_seq_take(&seq);
        link_batch_init(&b, buf, link_frame_payload(185), s, true);
        while (next < 5 && link_batch_add(&b, recs[next], lens[next])) {
            next++;
        }
        size_t len = link_batch_finish(&b);
        CHECK(len > 0 && len <= 182);
        CHECK(decode_frame(buf, len, &d) && d.count == 1 && d.seq == s);
        if (frame_no > 0) {
            CHECK(seq_gap(prev_seq, s) == 0);
        }
        prev_seq = s;
        char name[64];
        snprintf(name, sizeof(name), "records_mtu185_frame%d", frame_no++);
        fixture(name, buf, len);
    }
    CHECK(frame_no == 3);

    // Records at MTU 517, sequence 9, unsynced: everything in one frame.
    link_batch_init(&b, buf, link_frame_payload(517), 9, false);
    for (int i = 0; i < 5; i++) {
        CHECK(link_batch_add(&b, recs[i], lens[i]));
    }
    fixture("records_mtu517_seq9", buf, link_batch_finish(&b));

    // Exact fit: an AP record sized so the frame is exactly 182 bytes.
    load_record("record_ap", recs[0], sizeof(recs[0]), &r);
    size_t target = link_batch_max_record(182);
    CHECK(size_ap_exactly(&r, target));
    n = encode(ns_board_v1_Record_fields, &r, msg, sizeof(msg));
    link_batch_init(&b, buf, 182, 42, true);
    CHECK(link_batch_add(&b, msg, n));
    size_t len = link_batch_finish(&b);
    CHECK(len == 182);
    fixture("records_exact_fit_mtu185_seq42", buf, len);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fixtures-root> [--write]\n", argv[0]);
        return 2;
    }
    fixtures_root = argv[1];
    write_mode = argc > 2 && strcmp(argv[2], "--write") == 0;
    printf("link (%s)\n", write_mode ? "writing fixtures" : "checking fixtures");

    test_varint();
    test_payload_sizes();
    test_frame_boundaries();
    test_batch();
    test_seq_wrap();
    test_ring_basic();
    test_ring_overflow();
    test_ring_model();
    make_fixtures();

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
