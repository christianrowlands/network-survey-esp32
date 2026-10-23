// Host round-trip test for the nanopb board protocol code.
//
// Builds one synthetic message of each kind, encodes it with nanopb, decodes
// it back and checks the fields, then compares the bytes with the fixture in
// test/fixtures/proto/. The iOS BoardProtocol tests decode the same fixtures
// with SwiftProtobuf, which proves both sides agree on the wire format.
//
// Usage: proto_roundtrip <fixtures-dir> [--write]
//   --write  (re)write the fixture files instead of comparing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pb_decode.h"
#include "pb_encode.h"
#include "ns_board.pb.h"
#include "ns_proto_fit.h"

#define BUF_SIZE 1024

static int failures;
static const char *fixtures_dir;
static int write_mode;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// Synthetic, locally administered or clearly fake addresses only.
static const uint8_t AP_BSSID[6] = {0x02, 0x00, 0x5E, 0x10, 0x00, 0x01};
static const uint8_t CLIENT_MAC[6] = {0x0A, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t PROBE_MAC[6] = {0x0A, 0x66, 0x77, 0x88, 0x99, 0xAA};
static const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t BLE_ADDR[6] = {0xC2, 0x00, 0x5E, 0x00, 0x00, 0x01};
static const uint8_t BOARD_ADDR[6] = {0x02, 0x00, 0x5E, 0x00, 0xAB, 0xCD};

static const uint64_t T0 = 1767225600123ULL;  // 2026-01-01T00:00:00.123Z

static void set_bytes(pb_byte_t *dst, pb_size_t *size, const void *src, size_t n) {
    memcpy(dst, src, n);
    *size = (pb_size_t)n;
}

#define SET_BYTES(field, src, n) set_bytes((field).bytes, &(field).size, (src), (n))

// Encodes msg, decodes into out, re-encodes out and checks both encodings
// match, then writes or compares the fixture. Returns the encoded length.
static size_t roundtrip(const char *name, const pb_msgdesc_t *fields, const void *msg, void *out,
                        uint8_t *buf) {
    pb_ostream_t os = pb_ostream_from_buffer(buf, BUF_SIZE);
    if (!pb_encode(&os, fields, msg)) {
        fprintf(stderr, "%s: encode failed: %s\n", name, PB_GET_ERROR(&os));
        failures++;
        return 0;
    }
    size_t len = os.bytes_written;

    pb_istream_t is = pb_istream_from_buffer(buf, len);
    if (!pb_decode(&is, fields, out)) {
        fprintf(stderr, "%s: decode failed: %s\n", name, PB_GET_ERROR(&is));
        failures++;
        return 0;
    }

    uint8_t again[BUF_SIZE];
    pb_ostream_t os2 = pb_ostream_from_buffer(again, sizeof(again));
    CHECK(pb_encode(&os2, fields, out));
    CHECK(os2.bytes_written == len && memcmp(again, buf, len) == 0);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bin", fixtures_dir, name);
    if (write_mode) {
        FILE *f = fopen(path, "wb");
        if (!f || fwrite(buf, 1, len, f) != len) {
            fprintf(stderr, "%s: cannot write %s\n", name, path);
            failures++;
        }
        if (f) fclose(f);
    } else {
        uint8_t want[BUF_SIZE];
        FILE *f = fopen(path, "rb");
        size_t n = f ? fread(want, 1, sizeof(want), f) : 0;
        if (f) fclose(f);
        if (!f || n != len || memcmp(want, buf, len) != 0) {
            fprintf(stderr, "%s: fixture %s missing or different (run make fixtures)\n", name, path);
            failures++;
        }
    }
    printf("  %-28s %3zu bytes\n", name, len);
    return len;
}

static void fill_ap(ns_board_v1_ApObservation *ap) {
    static const uint8_t rates[] = {0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24};
    static const uint8_t ext_rates[] = {0x30, 0x48, 0x60, 0x6C};
    *ap = (ns_board_v1_ApObservation)ns_board_v1_ApObservation_init_zero;
    ap->board_time_ms = T0;
    ap->channel = 6;
    ap->frequency_mhz = 2437;
    ap->band = ns_board_v1_Band_BAND_GHZ_2_4;
    memcpy(ap->bssid, AP_BSSID, 6);
    SET_BYTES(ap->ssid, "ExampleNet", 10);
    ap->rssi_max = -42;
    ap->rssi_last = -47;
    ap->has_noise_floor_dbm = true;
    ap->noise_floor_dbm = -95;
    ap->beacon_count = 9;
    ap->probe_response_count = 2;
    ap->has_beacon_interval_tu = true;
    ap->beacon_interval_tu = 100;
    ap->capability_info = 0x0431;
    ap->security = ns_board_v1_SecurityType_SECURITY_TYPE_WPA2_WPA3;
    ap->group_cipher = 0x000FAC04;
    ap->pairwise_ciphers_count = 1;
    ap->pairwise_ciphers[0] = 0x000FAC04;
    ap->akm_suites_count = 2;
    ap->akm_suites[0] = 0x000FAC02;
    ap->akm_suites[1] = 0x000FAC08;
    ap->phy_flags = ns_board_v1_PhyFlag_PHY_FLAG_HT | ns_board_v1_PhyFlag_PHY_FLAG_VHT |
                    ns_board_v1_PhyFlag_PHY_FLAG_HE;
    ap->channel_width = ns_board_v1_ChannelWidth_CHANNEL_WIDTH_MHZ_40;
    ap->wps = true;
    ap->passpoint = false;
    SET_BYTES(ap->supported_rates, rates, sizeof(rates));
    SET_BYTES(ap->extended_supported_rates, ext_rates, sizeof(ext_rates));
    ap->has_station_count = true;
    ap->station_count = 5;
    ap->has_channel_utilization = true;
    ap->channel_utilization = 37;
}

static void fill_ble(ns_board_v1_BleObservation *ble) {
    // Flags, Complete Local Name "ExampleTag", Manufacturer data (0xFFFF), TX Power -12.
    static const uint8_t adv[] = {0x02, 0x01, 0x06, 0x0B, 0x09, 'E',  'x',  'a',
                                  'm',  'p',  'l',  'e',  'T',  'a',  'g',  0x05,
                                  0xFF, 0xFF, 0xFF, 0x01, 0x02, 0x02, 0x0A, 0xF4};
    *ble = (ns_board_v1_BleObservation)ns_board_v1_BleObservation_init_zero;
    ble->board_time_ms = T0 + 2000;
    ble->channel = 37;
    memcpy(ble->address, BLE_ADDR, 6);
    ble->address_type = ns_board_v1_BleAddressType_BLE_ADDRESS_TYPE_RANDOM;
    ble->rssi_max = -80;
    ble->rssi_last = -82;
    ble->count = 11;
    ble->adv_type = ns_board_v1_BleAdvType_BLE_ADV_TYPE_ADV_IND;
    SET_BYTES(ble->adv_data, adv, sizeof(adv));
    SET_BYTES(ble->local_name, "ExampleTag", 10);
    ble->has_company_id = true;
    ble->company_id = 0xFFFF;
    ble->has_tx_power = true;
    ble->tx_power = -12;
}

static void fill_gps(ns_board_v1_GpsFix *gps) {
    *gps = (ns_board_v1_GpsFix)ns_board_v1_GpsFix_init_zero;
    gps->board_time_ms = T0 + 3000;
    gps->latitude = 39.7392;
    gps->longitude = -104.9903;
    gps->has_altitude_m = true;
    gps->altitude_m = 1609.5f;
    gps->has_hdop = true;
    gps->hdop = 0.9f;
    gps->satellites = 9;
    gps->fix_time_ms = T0 + 2877;
    gps->fix_quality = 1;
    gps->has_speed_mps = true;
    gps->speed_mps = 12.5f;
    gps->has_course_deg = true;
    gps->course_deg = 270.0f;
}

static void test_records(uint8_t *buf) {
    static ns_board_v1_RecordBatch batch, out;

    // AP observation.
    batch = (ns_board_v1_RecordBatch)ns_board_v1_RecordBatch_init_zero;
    batch.time_synced = true;
    batch.records_count = 1;
    batch.records[0].which_kind = ns_board_v1_Record_ap_tag;
    fill_ap(&batch.records[0].kind.ap);
    roundtrip("record_ap", ns_board_v1_RecordBatch_fields, &batch, &out, buf);
    CHECK(out.time_synced && out.records_count == 1);
    CHECK(out.records[0].which_kind == ns_board_v1_Record_ap_tag);
    const ns_board_v1_ApObservation *ap = &out.records[0].kind.ap;
    CHECK(ap->board_time_ms == T0 && ap->channel == 6 && ap->frequency_mhz == 2437);
    CHECK(memcmp(ap->bssid, AP_BSSID, 6) == 0);
    CHECK(ap->ssid.size == 10 && memcmp(ap->ssid.bytes, "ExampleNet", 10) == 0);
    CHECK(ap->rssi_max == -42 && ap->rssi_last == -47);
    CHECK(ap->has_noise_floor_dbm && ap->noise_floor_dbm == -95);
    CHECK(ap->has_beacon_interval_tu && ap->beacon_interval_tu == 100);
    CHECK(ap->security == ns_board_v1_SecurityType_SECURITY_TYPE_WPA2_WPA3);
    CHECK(ap->akm_suites_count == 2 && ap->akm_suites[1] == 0x000FAC08);
    CHECK(ap->phy_flags == 7 && ap->wps && !ap->passpoint);
    CHECK(ap->supported_rates.size == 8 && ap->extended_supported_rates.size == 4);
    CHECK(ap->has_station_count && ap->station_count == 5);

    // Client observation.
    batch = (ns_board_v1_RecordBatch)ns_board_v1_RecordBatch_init_zero;
    batch.time_synced = true;
    batch.records_count = 1;
    batch.records[0].which_kind = ns_board_v1_Record_client_tag;
    ns_board_v1_ClientObservation *c = &batch.records[0].kind.client;
    c->board_time_ms = T0 + 500;
    c->channel = 36;
    c->frequency_mhz = 5180;
    c->band = ns_board_v1_Band_BAND_GHZ_5;
    memcpy(c->client_mac, CLIENT_MAC, 6);
    SET_BYTES(c->associated_bssid, AP_BSSID, 6);
    c->rssi_max = -60;
    c->rssi_last = -63;
    c->frame_count = 14;
    c->source_flags = ns_board_v1_ClientSource_CLIENT_SOURCE_DATA |
                      ns_board_v1_ClientSource_CLIENT_SOURCE_NULL_DATA |
                      ns_board_v1_ClientSource_CLIENT_SOURCE_ACTION;
    c->locally_administered = true;
    roundtrip("record_client", ns_board_v1_RecordBatch_fields, &batch, &out, buf);
    const ns_board_v1_ClientObservation *co = &out.records[0].kind.client;
    CHECK(out.records[0].which_kind == ns_board_v1_Record_client_tag);
    CHECK(memcmp(co->client_mac, CLIENT_MAC, 6) == 0 && co->associated_bssid.size == 6);
    CHECK(co->band == ns_board_v1_Band_BAND_GHZ_5 && co->source_flags == 67);
    CHECK(!co->has_noise_floor_dbm && co->locally_administered && co->frame_count == 14);

    // Probe observation.
    batch = (ns_board_v1_RecordBatch)ns_board_v1_RecordBatch_init_zero;
    batch.time_synced = true;
    batch.records_count = 1;
    batch.records[0].which_kind = ns_board_v1_Record_probe_tag;
    ns_board_v1_ProbeObservation *p = &batch.records[0].kind.probe;
    p->board_time_ms = T0 + 750;
    p->channel = 1;
    p->frequency_mhz = 2412;
    p->band = ns_board_v1_Band_BAND_GHZ_2_4;
    memcpy(p->client_mac, PROBE_MAC, 6);
    SET_BYTES(p->ssid, "Cafe-Guest", 10);
    p->rssi_max = -71;
    p->rssi_last = -75;
    p->count = 3;
    p->locally_administered = true;
    SET_BYTES(p->destination_address, BROADCAST, 6);
    SET_BYTES(p->bssid, BROADCAST, 6);
    p->phy_flags = ns_board_v1_PhyFlag_PHY_FLAG_HT;
    roundtrip("record_probe", ns_board_v1_RecordBatch_fields, &batch, &out, buf);
    const ns_board_v1_ProbeObservation *po = &out.records[0].kind.probe;
    CHECK(out.records[0].which_kind == ns_board_v1_Record_probe_tag);
    CHECK(po->ssid.size == 10 && memcmp(po->ssid.bytes, "Cafe-Guest", 10) == 0);
    CHECK(po->count == 3 && po->rssi_last == -75 && po->phy_flags == 1);
    CHECK(po->destination_address.size == 6 && po->bssid.size == 6);

    // BLE observation.
    batch = (ns_board_v1_RecordBatch)ns_board_v1_RecordBatch_init_zero;
    batch.time_synced = true;
    batch.records_count = 1;
    batch.records[0].which_kind = ns_board_v1_Record_ble_tag;
    fill_ble(&batch.records[0].kind.ble);
    roundtrip("record_ble", ns_board_v1_RecordBatch_fields, &batch, &out, buf);
    const ns_board_v1_BleObservation *b = &out.records[0].kind.ble;
    CHECK(out.records[0].which_kind == ns_board_v1_Record_ble_tag);
    CHECK(memcmp(b->address, BLE_ADDR, 6) == 0 && b->adv_data.size == 24);
    CHECK(b->local_name.size == 10 && memcmp(b->local_name.bytes, "ExampleTag", 10) == 0);
    CHECK(b->has_company_id && b->company_id == 0xFFFF && b->has_tx_power && b->tx_power == -12);

    // GPS fix.
    batch = (ns_board_v1_RecordBatch)ns_board_v1_RecordBatch_init_zero;
    batch.time_synced = true;
    batch.records_count = 1;
    batch.records[0].which_kind = ns_board_v1_Record_gps_tag;
    fill_gps(&batch.records[0].kind.gps);
    roundtrip("record_gps", ns_board_v1_RecordBatch_fields, &batch, &out, buf);
    const ns_board_v1_GpsFix *g = &out.records[0].kind.gps;
    CHECK(out.records[0].which_kind == ns_board_v1_Record_gps_tag);
    CHECK(g->latitude == 39.7392 && g->longitude == -104.9903);
    CHECK(g->has_altitude_m && g->altitude_m == 1609.5f && g->satellites == 9);
    CHECK(g->fix_quality == 1 && g->has_course_deg && g->course_deg == 270.0f);

    // Mixed batch before time sync: times are ms since boot.
    batch = (ns_board_v1_RecordBatch)ns_board_v1_RecordBatch_init_zero;
    batch.time_synced = false;
    batch.records_count = 3;
    batch.records[0].which_kind = ns_board_v1_Record_ap_tag;
    fill_ap(&batch.records[0].kind.ap);
    batch.records[0].kind.ap.board_time_ms = 15000;
    batch.records[1].which_kind = ns_board_v1_Record_ble_tag;
    fill_ble(&batch.records[1].kind.ble);
    batch.records[1].kind.ble.board_time_ms = 15250;
    batch.records[2].which_kind = ns_board_v1_Record_gps_tag;
    fill_gps(&batch.records[2].kind.gps);
    batch.records[2].kind.gps.board_time_ms = 15500;
    roundtrip("record_batch_mixed", ns_board_v1_RecordBatch_fields, &batch, &out, buf);
    CHECK(!out.time_synced && out.records_count == 3);
    CHECK(out.records[1].which_kind == ns_board_v1_Record_ble_tag);
    CHECK(out.records[2].kind.gps.board_time_ms == 15500);
}

static void test_board_messages(uint8_t *buf) {
    ns_board_v1_BoardInfo info = ns_board_v1_BoardInfo_init_zero, info_out;
    info.protocol_major = ns_board_v1_ProtocolVersion_PROTOCOL_VERSION_MAJOR;
    info.protocol_minor = ns_board_v1_ProtocolVersion_PROTOCOL_VERSION_MINOR;
    strcpy(info.firmware_version, "0.1.0");
    info.board_model = ns_board_v1_BoardModel_BOARD_MODEL_ESP32_C5_DEVKITC_1;
    info.chip = ns_board_v1_Chip_CHIP_ESP32_C5;
    info.bands_count = 2;
    info.bands[0] = ns_board_v1_Band_BAND_GHZ_2_4;
    info.bands[1] = ns_board_v1_Band_BAND_GHZ_5;
    info.gps_present = false;
    strcpy(info.short_id, "ABCD");
    info.preferred_mtu = 517;
    memcpy(info.bt_address, BOARD_ADDR, 6);
    strcpy(info.model_name, "ESP32-C5-DevKitC-1");
    roundtrip("board_info", ns_board_v1_BoardInfo_fields, &info, &info_out, buf);
    CHECK(info_out.protocol_major == 1 && info_out.bands_count == 2);
    CHECK(strcmp(info_out.short_id, "ABCD") == 0 && info_out.preferred_mtu == 517);

    ns_board_v1_BoardStatus st = ns_board_v1_BoardStatus_init_zero, st_out;
    st.board_time_ms = T0 + 10000;
    st.time_synced = true;
    st.state = ns_board_v1_BoardState_BOARD_STATE_SURVEYING;
    st.role = ns_board_v1_Role_ROLE_BAND_5;
    st.current_channel = 149;
    st.records_per_sec = 42;
    st.ring_fill_percent = 12;
    st.dropped_records = 3;
    st.dropped_raw_frames = 0;
    st.dropped_rx_frames = 17;
    st.rx_frames_per_sec = 850;
    st.uptime_ms = 3600000;
    st.free_heap_bytes = 180000;
    st.min_free_heap_bytes = 150000;
    st.gps_state = ns_board_v1_GpsState_GPS_STATE_NOT_PRESENT;
    st.has_battery_mv = true;
    st.battery_mv = 3900;
    st.pairing_mode = false;
    st.bond_count = 1;
    st.mtu = 247;
    roundtrip("board_status", ns_board_v1_BoardStatus_fields, &st, &st_out, buf);
    CHECK(st_out.state == ns_board_v1_BoardState_BOARD_STATE_SURVEYING && st_out.current_channel == 149);
    CHECK(st_out.has_battery_mv && st_out.battery_mv == 3900 && st_out.mtu == 247);

    ns_board_v1_ControlRequest req = ns_board_v1_ControlRequest_init_zero, req_out;
    req.request_id = 7;
    req.which_command = ns_board_v1_ControlRequest_configure_tag;
    ns_board_v1_Configure *cfg = &req.command.configure;
    cfg->wifi_ap = true;
    cfg->wifi_client = true;
    cfg->wifi_probes = true;
    cfg->ble = false;
    cfg->raw_frames = true;
    cfg->role = ns_board_v1_Role_ROLE_AUTO;
    static const uint32_t channels[] = {1, 6, 11, 36, 40, 44, 48, 149};
    cfg->channels_count = sizeof(channels) / sizeof(channels[0]);
    memcpy(cfg->channels, channels, sizeof(channels));
    cfg->dwell_24_ms = 250;
    cfg->dwell_5_ms = 150;
    cfg->raw_snaplen = 128;
    cfg->raw_max_frames_per_sec = 50;
    cfg->raw_type_mask = 1;
    roundtrip("control_request_configure", ns_board_v1_ControlRequest_fields, &req, &req_out, buf);
    CHECK(req_out.request_id == 7 && req_out.which_command == ns_board_v1_ControlRequest_configure_tag);
    CHECK(req_out.command.configure.channels_count == 8 && req_out.command.configure.channels[7] == 149);

    ns_board_v1_ControlRequest hello = ns_board_v1_ControlRequest_init_zero;
    hello.request_id = 1;
    hello.which_command = ns_board_v1_ControlRequest_hello_tag;
    hello.command.hello.protocol_major = 1;
    hello.command.hello.protocol_minor = 0;
    strcpy(hello.command.hello.app_version, "1.0.0");
    roundtrip("control_request_hello", ns_board_v1_ControlRequest_fields, &hello, &req_out, buf);
    CHECK(req_out.which_command == ns_board_v1_ControlRequest_hello_tag);
    CHECK(strcmp(req_out.command.hello.app_version, "1.0.0") == 0);

    ns_board_v1_ControlResponse resp = ns_board_v1_ControlResponse_init_zero, resp_out;
    resp.request_id = 1;
    resp.result = ns_board_v1_ResultCode_RESULT_CODE_OK;
    resp.which_payload = ns_board_v1_ControlResponse_hello_tag;
    resp.payload.hello.protocol_major = 1;
    resp.payload.hello.protocol_minor = 0;
    resp.payload.hello.compatible = true;
    strcpy(resp.payload.hello.firmware_version, "0.1.0");
    resp.payload.hello.ota_image_confirmed = true;
    roundtrip("control_response_hello", ns_board_v1_ControlResponse_fields, &resp, &resp_out, buf);
    CHECK(resp_out.result == ns_board_v1_ResultCode_RESULT_CODE_OK);
    CHECK(resp_out.which_payload == ns_board_v1_ControlResponse_hello_tag && resp_out.payload.hello.compatible);

    // A beacon header (frame control, duration, addresses, sequence) cut at a 40-byte snaplen.
    static const uint8_t frame[40] = {
        0x80, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x00, 0x5E, 0x10,
        0x00, 0x01, 0x02, 0x00, 0x5E, 0x10, 0x00, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x31, 0x04, 0x00, 0x0A, 'E',  'x'};
    static ns_board_v1_RawFrame raw, raw_out;
    raw = (ns_board_v1_RawFrame)ns_board_v1_RawFrame_init_zero;
    raw.board_time_us = T0 * 1000 + 456;
    raw.time_synced = true;
    raw.channel = 6;
    raw.frequency_mhz = 2437;
    raw.band = ns_board_v1_Band_BAND_GHZ_2_4;
    raw.rssi = -44;
    raw.has_noise_floor_dbm = true;
    raw.noise_floor_dbm = -96;
    raw.phy = ns_board_v1_FramePhy_FRAME_PHY_LEGACY;
    raw.rate_500kbps = 2;
    raw.orig_len = 180;
    raw.fcs_included = false;
    SET_BYTES(raw.data, frame, sizeof(frame));
    roundtrip("raw_frame", ns_board_v1_RawFrame_fields, &raw, &raw_out, buf);
    CHECK(raw_out.orig_len == 180 && raw_out.data.size == 40 && raw_out.data.bytes[0] == 0x80);
    CHECK(!raw_out.has_mcs && raw_out.rate_500kbps == 2);

    ns_board_v1_OtaAck ack = ns_board_v1_OtaAck_init_zero, ack_out;
    ack.next_offset = 65536;
    ack.status = ns_board_v1_OtaAckStatus_OTA_ACK_STATUS_OK;
    roundtrip("ota_ack", ns_board_v1_OtaAck_fields, &ack, &ack_out, buf);
    CHECK(ack_out.next_offset == 65536 && ack_out.status == ns_board_v1_OtaAckStatus_OTA_ACK_STATUS_OK);
}

// The largest ApObservation a board can produce: every list full, every
// optional field present, every value at the top of its valid range.
static void fill_max_ap(ns_board_v1_ApObservation *ap) {
    *ap = (ns_board_v1_ApObservation)ns_board_v1_ApObservation_init_zero;
    ap->board_time_ms = 4102444800000ULL;  // 2100-01-01
    ap->channel = 177;
    ap->frequency_mhz = 5885;
    ap->band = ns_board_v1_Band_BAND_GHZ_5;
    memcpy(ap->bssid, AP_BSSID, 6);
    ap->ssid.size = 32;
    memset(ap->ssid.bytes, 'S', 32);
    ap->rssi_max = -128;
    ap->rssi_last = -128;
    ap->has_noise_floor_dbm = true;
    ap->noise_floor_dbm = -128;
    ap->beacon_count = UINT32_MAX;
    ap->probe_response_count = UINT32_MAX;
    ap->has_beacon_interval_tu = true;
    ap->beacon_interval_tu = 0xFFFF;
    ap->capability_info = 0xFFFF;
    ap->security = ns_board_v1_SecurityType_SECURITY_TYPE_WPA2_WPA3;
    ap->group_cipher = UINT32_MAX;
    ap->pairwise_ciphers_count = 8;
    ap->akm_suites_count = 8;
    for (int i = 0; i < 8; i++) {
        ap->pairwise_ciphers[i] = UINT32_MAX;
        ap->akm_suites[i] = UINT32_MAX;
    }
    ap->phy_flags = 0xF;
    ap->channel_width = ns_board_v1_ChannelWidth_CHANNEL_WIDTH_MHZ_320;
    ap->wps = true;
    ap->passpoint = true;
    ap->supported_rates.size = 16;
    memset(ap->supported_rates.bytes, 0xEC, 16);
    ap->extended_supported_rates.size = 32;
    memset(ap->extended_supported_rates.bytes, 0xEC, 32);
    ap->has_station_count = true;
    ap->station_count = 0xFFFF;
    ap->has_channel_utilization = true;
    ap->channel_utilization = 255;
}

// Records must fit one notification at the smallest supported MTU (185).
static void test_fit(void) {
    CHECK(ns_proto_max_record_size(185) == 173);
    CHECK(ns_proto_max_record_size(517) == 505);
    CHECK(ns_proto_max_record_size(12) == 0);

    static ns_board_v1_Record rec;
    const size_t limit = ns_proto_max_record_size(185);

    // A typical AP fits untouched.
    rec.which_kind = ns_board_v1_Record_ap_tag;
    fill_ap(&rec.kind.ap);
    size_t before = ns_proto_record_size(&rec);
    CHECK(ns_proto_fit_record(&rec, limit));
    CHECK(!rec.kind.ap.detail_trimmed && ns_proto_record_size(&rec) == before);

    // The largest AP is trimmed until it fits; identity fields survive.
    rec.which_kind = ns_board_v1_Record_ap_tag;
    fill_max_ap(&rec.kind.ap);
    before = ns_proto_record_size(&rec);
    CHECK(before > limit);
    CHECK(ns_proto_fit_record(&rec, limit));
    size_t after = ns_proto_record_size(&rec);
    CHECK(after <= limit);
    const ns_board_v1_ApObservation *ap = &rec.kind.ap;
    CHECK(ap->detail_trimmed && ap->ssid.size == 32 && memcmp(ap->bssid, AP_BSSID, 6) == 0);
    CHECK(ap->extended_supported_rates.size == 0 && !ap->has_station_count);
    CHECK(ap->pairwise_ciphers_count >= 1 && ap->akm_suites_count >= 1);
    printf("  %-28s %3zu -> %zu bytes (limit %zu)\n", "fit max ap", before, after, limit);

    // The whole notification (sequence, length, batch) fits in ATT_MTU - 3.
    static ns_board_v1_RecordBatch batch;
    batch = (ns_board_v1_RecordBatch)ns_board_v1_RecordBatch_init_zero;
    batch.time_synced = true;
    batch.records_count = 1;
    batch.records[0] = rec;
    size_t batch_size = 0;
    CHECK(pb_get_encoded_size(&batch_size, ns_board_v1_RecordBatch_fields, &batch));
    CHECK(2 + (batch_size < 128 ? 1 : 2) + batch_size <= 185 - 3);

    // Every other record kind fits untrimmed at the largest valid size.
    rec.which_kind = ns_board_v1_Record_ble_tag;
    fill_ble(&rec.kind.ble);
    rec.kind.ble.adv_data.size = 31;
    rec.kind.ble.local_name.size = 31;
    rec.kind.ble.board_time_ms = 4102444800000ULL;
    rec.kind.ble.count = UINT32_MAX;
    CHECK(ns_proto_record_size(&rec) <= limit && ns_proto_fit_record(&rec, limit));

    rec.which_kind = ns_board_v1_Record_probe_tag;
    rec.kind.probe = (ns_board_v1_ProbeObservation)ns_board_v1_ProbeObservation_init_zero;
    rec.kind.probe.board_time_ms = 4102444800000ULL;
    rec.kind.probe.ssid.size = 32;
    rec.kind.probe.destination_address.size = 6;
    rec.kind.probe.bssid.size = 6;
    rec.kind.probe.count = UINT32_MAX;
    rec.kind.probe.phy_flags = 0xF;
    CHECK(ns_proto_record_size(&rec) <= limit);

    // Below the supported MTU a large AP cannot fit and is reported as such.
    rec.which_kind = ns_board_v1_Record_ap_tag;
    fill_max_ap(&rec.kind.ap);
    CHECK(!ns_proto_fit_record(&rec, ns_proto_max_record_size(100)));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fixtures-dir> [--write]\n", argv[0]);
        return 2;
    }
    fixtures_dir = argv[1];
    write_mode = argc > 2 && strcmp(argv[2], "--write") == 0;

    static uint8_t buf[BUF_SIZE];
    printf("proto_roundtrip (%s)\n", write_mode ? "writing fixtures" : "checking fixtures");
    test_records(buf);
    test_board_messages(buf);
    test_fit();

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
