// Host tests for dot11_parser: synthetic frames for every frame kind and
// element the parser reads, plus truncation and garbage input (built with
// AddressSanitizer so any out-of-bounds read fails the run).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/frame_builder.h"
#include "dot11_parser.h"

static int s_failures;
static int s_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        s_checks++;                                                          \
        if (!(cond)) {                                                       \
            s_failures++;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b) CHECK((long long)(a) == (long long)(b))

#define RSN(t) (0x000FAC00u | (t))
#define WPA(t) (0x0050F200u | (t))

static dot11_frame_t parse(const fb_t *f)
{
    dot11_frame_t out;
    // Exact-size heap copy so AddressSanitizer catches any read past the end.
    uint8_t *copy = malloc(f->len ? f->len : 1);
    memcpy(copy, f->buf, f->len);
    dot11_parse(copy, f->len, &out);
    free(copy);
    return out;
}

static void test_full_beacon(void)
{
    fb_t f;
    fb_full_beacon(&f);
    dot11_frame_t p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_BEACON);
    CHECK(!p.truncated);
    CHECK(p.fixed_fields);
    CHECK(memcmp(p.addr3, FB_AP1, 6) == 0);
    CHECK_EQ(p.beacon_interval_tu, 100);
    CHECK_EQ(p.capability, 0x0411);
    CHECK(p.ies.ssid_present && !p.ies.ssid_hidden);
    CHECK_EQ(p.ies.ssid_len, 10);
    CHECK(memcmp(p.ies.ssid, "ExampleNet", 10) == 0);
    CHECK_EQ(p.ies.rates_len, 8);
    CHECK_EQ(p.ies.rates[0], 0x82);
    CHECK_EQ(p.ies.ext_rates_len, 4);
    CHECK_EQ(p.ies.ext_rates[3], 0x6C);
    CHECK_EQ(p.ies.ds_channel, 6);
    CHECK_EQ(p.ies.ht_primary_channel, 6);
    CHECK(p.ies.bss_load);
    CHECK_EQ(p.ies.station_count, 5);
    CHECK_EQ(p.ies.channel_utilization, 40);
    CHECK_EQ(p.ies.phy_flags, DOT11_PHY_HT | DOT11_PHY_HE);
    CHECK_EQ(p.ies.width, DOT11_WIDTH_40);
    CHECK(p.ies.rsn && !p.ies.wpa);
    CHECK_EQ(p.ies.group_cipher, RSN(4));
    CHECK_EQ(p.ies.pairwise_count, 1);
    CHECK_EQ(p.ies.pairwise[0], RSN(4));
    CHECK_EQ(p.ies.akm_count, 2);
    CHECK_EQ(p.ies.akm[0], RSN(2));
    CHECK_EQ(p.ies.akm[1], RSN(8));
    CHECK_EQ(p.security, DOT11_SEC_WPA2_WPA3);
    CHECK(p.ies.wps);
    CHECK(p.ies.interworking);
    CHECK(p.ies.passpoint);
}

static void test_probe_response(void)
{
    fb_t f;
    fb_beacon(&f, 5, FB_AP2, 0x0001);
    fb_ssid(&f, "Cafe-Guest");
    dot11_frame_t p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_PROBE_RESP);
    CHECK(memcmp(p.addr1, FB_STA1, 6) == 0);
    CHECK(memcmp(p.addr3, FB_AP2, 6) == 0);
    CHECK_EQ(p.security, DOT11_SEC_OPEN);
    CHECK_EQ(p.ies.width, DOT11_WIDTH_UNKNOWN);
    CHECK_EQ(p.ies.ds_channel, 0);
}

static void test_hidden_ssid(void)
{
    fb_t f;
    fb_beacon(&f, 8, FB_AP1, 0x0001);
    fb_ie(&f, 0, "", 0);
    dot11_frame_t p = parse(&f);
    CHECK(p.ies.ssid_present && p.ies.ssid_hidden);
    CHECK_EQ(p.ies.ssid_len, 0);

    static const uint8_t zeros[8] = {0};
    fb_beacon(&f, 8, FB_AP1, 0x0001);
    fb_ie(&f, 0, zeros, sizeof(zeros));
    p = parse(&f);
    CHECK(p.ies.ssid_present && p.ies.ssid_hidden);
    CHECK_EQ(p.ies.ssid_len, 8);

    // Only the first SSID element counts.
    fb_beacon(&f, 8, FB_AP1, 0x0001);
    fb_ssid(&f, "ExampleNet");
    fb_ssid(&f, "Other");
    p = parse(&f);
    CHECK_EQ(p.ies.ssid_len, 10);

    // An SSID element longer than 32 bytes is malformed and ignored.
    static const uint8_t long_ssid[33] = {'A'};
    fb_beacon(&f, 8, FB_AP1, 0x0001);
    fb_ie(&f, 0, long_ssid, sizeof(long_ssid));
    p = parse(&f);
    CHECK(!p.ies.ssid_present);
}

typedef struct {
    const char *name;
    uint16_t capability;
    int rsn_akm_count;
    uint32_t rsn_akm[3];
    bool wpa;
    dot11_security_t expect;
} sec_case_t;

static void test_security(void)
{
    static const sec_case_t cases[] = {
        {"open", 0x0001, -1, {0}, false, DOT11_SEC_OPEN},
        {"wep", 0x0011, -1, {0}, false, DOT11_SEC_WEP},
        {"wpa", 0x0011, -1, {0}, true, DOT11_SEC_WPA},
        {"wpa2 psk", 0x0011, 1, {RSN(2)}, false, DOT11_SEC_WPA2},
        {"wpa2 enterprise", 0x0011, 1, {RSN(1)}, false, DOT11_SEC_WPA2},
        {"wpa2 psk-sha256", 0x0011, 1, {RSN(6)}, false, DOT11_SEC_WPA2},
        {"wpa/wpa2", 0x0011, 1, {RSN(2)}, true, DOT11_SEC_WPA_WPA2},
        {"wpa3 sae", 0x0011, 1, {RSN(8)}, false, DOT11_SEC_WPA3},
        {"wpa3 ft-sae", 0x0011, 1, {RSN(9)}, false, DOT11_SEC_WPA3},
        {"wpa3 sae-ext", 0x0011, 1, {RSN(24)}, false, DOT11_SEC_WPA3},
        {"wpa3 suite-b", 0x0011, 1, {RSN(12)}, false, DOT11_SEC_WPA3},
        {"wpa3 sae + wpa", 0x0011, 1, {RSN(8)}, true, DOT11_SEC_WPA3},
        {"transition", 0x0011, 2, {RSN(2), RSN(8)}, false, DOT11_SEC_WPA2_WPA3},
        {"transition + wpa", 0x0011, 2, {RSN(8), RSN(4)}, true, DOT11_SEC_WPA2_WPA3},
        {"owe", 0x0011, 1, {RSN(18)}, false, DOT11_SEC_OWE},
        {"owe + psk", 0x0011, 2, {RSN(18), RSN(2)}, false, DOT11_SEC_WPA2},
        {"rsn no akm", 0x0011, 0, {0}, false, DOT11_SEC_WPA2},
        {"rsn vendor akm", 0x0011, 1, {0x00112233u}, false, DOT11_SEC_WPA2},
    };
    const uint32_t pw[] = {RSN(4)};
    const uint32_t wpa_pw[] = {WPA(2)};
    const uint32_t wpa_akm[] = {WPA(2)};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const sec_case_t *c = &cases[i];
        fb_t f;
        fb_beacon(&f, 8, FB_AP1, c->capability);
        fb_ssid(&f, "ExampleNet");
        if (c->rsn_akm_count >= 0) {
            fb_rsn(&f, RSN(4), pw, 1, c->rsn_akm, c->rsn_akm_count);
        }
        if (c->wpa) {
            fb_wpa(&f, WPA(2), wpa_pw, 1, wpa_akm, 1);
        }
        dot11_frame_t p = parse(&f);
        if (p.security != c->expect) {
            fprintf(stderr, "security case '%s': got %d want %d\n", c->name, p.security, c->expect);
        }
        CHECK_EQ(p.security, c->expect);
    }
}

static void test_suite_lists(void)
{
    // RSN and WPA together: RSN suites first, RSN group cipher, duplicates removed.
    const uint32_t pw[] = {RSN(4), RSN(2), RSN(4)};
    const uint32_t akm[] = {RSN(2)};
    const uint32_t wpa_pw[] = {WPA(2), WPA(4)};
    const uint32_t wpa_akm[] = {WPA(2)};
    fb_t f;
    fb_beacon(&f, 8, FB_AP1, 0x0011);
    fb_wpa(&f, WPA(2), wpa_pw, 2, wpa_akm, 1);  // WPA element before RSN on purpose
    fb_rsn(&f, RSN(2), pw, 3, akm, 1);
    dot11_frame_t p = parse(&f);
    CHECK(p.ies.rsn && p.ies.wpa);
    CHECK_EQ(p.ies.group_cipher, RSN(2));
    CHECK_EQ(p.ies.pairwise_count, 4);
    CHECK_EQ(p.ies.pairwise[0], RSN(4));
    CHECK_EQ(p.ies.pairwise[1], RSN(2));
    CHECK_EQ(p.ies.pairwise[2], WPA(2));
    CHECK_EQ(p.ies.pairwise[3], WPA(4));
    CHECK_EQ(p.ies.akm_count, 2);
    CHECK_EQ(p.ies.akm[1], WPA(2));
    CHECK_EQ(p.security, DOT11_SEC_WPA_WPA2);

    // More than 8 suites: capped.
    uint32_t many[12];
    for (int i = 0; i < 12; i++) {
        many[i] = RSN(i + 1);
    }
    fb_beacon(&f, 8, FB_AP1, 0x0011);
    fb_rsn(&f, RSN(4), many, 12, many, 12);
    p = parse(&f);
    CHECK_EQ(p.ies.pairwise_count, DOT11_SUITES_MAX);
    CHECK_EQ(p.ies.akm_count, DOT11_SUITES_MAX);

    // SAE listed after the 8-suite cap still counts for the summary.
    uint32_t late_sae[10];
    for (int i = 0; i < 9; i++) {
        late_sae[i] = 0x00112200u | (uint32_t)i;  // vendor AKMs
    }
    late_sae[9] = RSN(8);
    fb_beacon(&f, 8, FB_AP1, 0x0011);
    fb_rsn(&f, RSN(4), pw, 1, late_sae, 10);
    p = parse(&f);
    CHECK_EQ(p.ies.akm_count, DOT11_SUITES_MAX);
    CHECK_EQ(p.security, DOT11_SEC_WPA3);
    late_sae[0] = RSN(2);
    fb_beacon(&f, 8, FB_AP1, 0x0011);
    fb_rsn(&f, RSN(4), pw, 1, late_sae, 10);
    p = parse(&f);
    CHECK_EQ(p.security, DOT11_SEC_WPA2_WPA3);

    // WPA only: group cipher from WPA.
    fb_beacon(&f, 8, FB_AP1, 0x0011);
    fb_wpa(&f, WPA(2), wpa_pw, 1, wpa_akm, 1);
    p = parse(&f);
    CHECK_EQ(p.ies.group_cipher, WPA(2));
    CHECK_EQ(p.security, DOT11_SEC_WPA);
}

static void test_rsn_partial(void)
{
    fb_t f;
    // Version only: RSN present, no suites (defaults apply), still WPA2.
    static const uint8_t version_only[] = {1, 0};
    fb_beacon(&f, 8, FB_AP1, 0x0011);
    fb_ie(&f, 48, version_only, sizeof(version_only));
    dot11_frame_t p = parse(&f);
    CHECK(p.ies.rsn);
    CHECK_EQ(p.ies.group_cipher, 0);
    CHECK_EQ(p.security, DOT11_SEC_WPA2);

    // Pairwise count claims 5 suites but the element holds 1: keep the one.
    static const uint8_t short_list[] = {1, 0, 0x00, 0x0F, 0xAC, 4, 5, 0, 0x00, 0x0F, 0xAC, 4};
    fb_beacon(&f, 8, FB_AP1, 0x0011);
    fb_ie(&f, 48, short_list, sizeof(short_list));
    fb_ssid(&f, "ExampleNet");
    p = parse(&f);
    CHECK(p.ies.rsn);
    CHECK_EQ(p.ies.pairwise_count, 1);
    CHECK_EQ(p.ies.akm_count, 0);
    CHECK(p.ies.ssid_present);  // elements after it still parse

    // Too short to hold a version: ignored.
    static const uint8_t one[] = {1};
    fb_beacon(&f, 8, FB_AP1, 0x0011);
    fb_ie(&f, 48, one, sizeof(one));
    p = parse(&f);
    CHECK(!p.ies.rsn);
    CHECK_EQ(p.security, DOT11_SEC_WEP);
}

static void test_widths(void)
{
    static const uint8_t ht_op_20[22] = {36, 0x00};
    static const uint8_t ht_op_40below[22] = {40, 0x07};
    static const uint8_t vht_cap[12] = {0};
    static const uint8_t vht_80[5] = {1, 42, 0, 0, 0};
    static const uint8_t vht_160[5] = {1, 42, 50, 0, 0};
    static const uint8_t vht_80p80[5] = {1, 42, 106, 0, 0};
    static const uint8_t vht_old_160[5] = {2, 50, 0, 0, 0};
    static const uint8_t vht_old_80p80[5] = {3, 42, 106, 0, 0};
    static const uint8_t eht_cap[10] = {0};
    static const uint8_t eht_op_320[8] = {0x01, 0, 0, 0, 0, 0x04, 47, 31};
    static const uint8_t eht_op_noinfo[5] = {0x00, 0, 0, 0, 0};
    static const uint8_t eht_op_short[5] = {0x01, 0, 0, 0, 0};
    struct {
        const uint8_t *ht;
        const uint8_t *vht;
        const uint8_t *eht;
        uint8_t eht_len;
        dot11_width_t expect;
    } cases[] = {
        {ht_op_20, NULL, NULL, 0, DOT11_WIDTH_20},
        {ht_op_40below, NULL, NULL, 0, DOT11_WIDTH_40},
        {ht_op_40below, vht_80, NULL, 0, DOT11_WIDTH_80},
        {ht_op_40below, vht_160, NULL, 0, DOT11_WIDTH_160},
        {ht_op_40below, vht_80p80, NULL, 0, DOT11_WIDTH_80P80},
        {ht_op_40below, vht_old_160, NULL, 0, DOT11_WIDTH_160},
        {ht_op_40below, vht_old_80p80, NULL, 0, DOT11_WIDTH_80P80},
        {ht_op_40below, vht_80, eht_op_320, sizeof(eht_op_320), DOT11_WIDTH_320},
        {ht_op_40below, vht_80, eht_op_noinfo, sizeof(eht_op_noinfo), DOT11_WIDTH_80},
        {ht_op_40below, vht_80, eht_op_short, sizeof(eht_op_short), DOT11_WIDTH_80},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fb_t f;
        fb_beacon(&f, 8, FB_AP1, 0x0001);
        fb_ssid(&f, "ExampleNet");
        fb_ie(&f, 61, cases[i].ht, 22);
        if (cases[i].vht) {
            fb_ie(&f, 191, vht_cap, sizeof(vht_cap));
            fb_ie(&f, 192, cases[i].vht, 5);
        }
        if (cases[i].eht) {
            fb_ext(&f, 108, eht_cap, sizeof(eht_cap));
            fb_ext(&f, 106, cases[i].eht, cases[i].eht_len);
        }
        dot11_frame_t p = parse(&f);
        if (p.ies.width != cases[i].expect) {
            fprintf(stderr, "width case %zu: got %d want %d\n", i, p.ies.width, cases[i].expect);
        }
        CHECK_EQ(p.ies.width, cases[i].expect);
        CHECK_EQ(p.ies.ht_primary_channel, cases[i].ht[0]);
        if (cases[i].eht) {
            CHECK_EQ(p.ies.phy_flags, DOT11_PHY_VHT | DOT11_PHY_EHT);
        }
    }
}

static void test_probe_request(void)
{
    static const uint8_t rates[] = {0x02, 0x04, 0x0B, 0x16};
    static const uint8_t ht_cap[26] = {0};
    static const uint8_t vht_cap[12] = {0};
    static const uint8_t he_cap[22] = {0};
    fb_t f;
    fb_header(&f, FB_FC(0, 4), 0, FB_BCAST, FB_STA1, FB_BCAST);
    fb_ie(&f, 0, "", 0);
    fb_ie(&f, 1, rates, sizeof(rates));
    fb_ie(&f, 45, ht_cap, sizeof(ht_cap));
    fb_ie(&f, 191, vht_cap, sizeof(vht_cap));
    fb_ext(&f, 35, he_cap, sizeof(he_cap));
    dot11_frame_t p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_PROBE_REQ);
    CHECK(p.ies.ssid_present);
    CHECK_EQ(p.ies.ssid_len, 0);
    CHECK_EQ(p.ies.phy_flags, DOT11_PHY_HT | DOT11_PHY_VHT | DOT11_PHY_HE);
    CHECK(memcmp(p.addr2, FB_STA1, 6) == 0);
    CHECK(dot11_is_local(p.addr2));
    CHECK(dot11_is_group(p.addr1));
    CHECK_EQ(p.security, DOT11_SEC_UNKNOWN);

    // Directed probe.
    fb_header(&f, FB_FC(0, 4), 0, FB_AP1, FB_STA2, FB_AP1);
    fb_ssid(&f, "ExampleNet");
    p = parse(&f);
    CHECK_EQ(p.ies.ssid_len, 10);
    CHECK(!dot11_is_local(p.addr2));
    CHECK(memcmp(p.addr1, FB_AP1, 6) == 0);
}

static void test_assoc_auth_action(void)
{
    fb_t f;
    fb_header(&f, FB_FC(0, 0), 0, FB_AP1, FB_STA1, FB_AP1);
    fb_le16(&f, 0x0431);  // capability
    fb_le16(&f, 10);      // listen interval
    fb_ssid(&f, "ExampleNet");
    dot11_frame_t p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_ASSOC_REQ);
    CHECK_EQ(p.capability, 0x0431);
    CHECK_EQ(p.ies.ssid_len, 10);
    CHECK(memcmp(p.addr3, FB_AP1, 6) == 0);

    fb_header(&f, FB_FC(0, 2), 0, FB_AP2, FB_STA1, FB_AP2);
    fb_le16(&f, 0x0431);
    fb_le16(&f, 10);
    fb_bytes(&f, FB_AP1, 6);  // current AP
    fb_ssid(&f, "ExampleNet");
    p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_REASSOC_REQ);
    CHECK(memcmp(p.current_ap, FB_AP1, 6) == 0);
    CHECK_EQ(p.ies.ssid_len, 10);

    fb_header(&f, FB_FC(0, 11), 0, FB_AP1, FB_STA1, FB_AP1);
    fb_le16(&f, 3);  // SAE
    fb_le16(&f, 1);
    fb_le16(&f, 0);
    p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_AUTH);
    CHECK_EQ(p.auth_algorithm, 3);
    CHECK_EQ(p.auth_seq, 1);

    fb_header(&f, FB_FC(0, 13), 0, FB_AP1, FB_STA1, FB_AP1);
    fb_u8(&f, 4);  // public action
    fb_u8(&f, 10);
    p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_ACTION);
    fb_header(&f, FB_FC(0, 14), 0, FB_AP1, FB_STA1, FB_AP1);
    p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_ACTION);

    // Subtypes we do not summarize (association response).
    fb_header(&f, FB_FC(0, 1), 0, FB_STA1, FB_AP1, FB_AP1);
    p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_OTHER);
}

static void test_data(void)
{
    fb_t f;
    // To DS: addr1 BSSID, addr2 station.
    fb_header(&f, FB_FC(2, 8), 0x01, FB_AP1, FB_STA1, FB_BCAST);
    fb_le16(&f, 0);  // QoS control
    dot11_frame_t p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_DATA);
    CHECK_EQ(p.dir, DOT11_DIR_TO_AP);
    CHECK(p.has_client && p.has_bssid);
    CHECK(memcmp(p.client, FB_STA1, 6) == 0);
    CHECK(memcmp(p.bssid, FB_AP1, 6) == 0);

    // From DS: addr1 station, addr2 BSSID.
    fb_header(&f, FB_FC(2, 0), 0x02, FB_STA2, FB_AP1, FB_AP2);
    p = parse(&f);
    CHECK_EQ(p.dir, DOT11_DIR_FROM_AP);
    CHECK(memcmp(p.client, FB_STA2, 6) == 0);
    CHECK(memcmp(p.bssid, FB_AP1, 6) == 0);

    // From DS to a group address: no client.
    fb_header(&f, FB_FC(2, 0), 0x02, FB_BCAST, FB_AP1, FB_AP2);
    p = parse(&f);
    CHECK(!p.has_client && p.has_bssid);

    // Neither bit: IBSS or direct link, addr3 is the BSSID.
    fb_header(&f, FB_FC(2, 0), 0x00, FB_STA2, FB_STA1, FB_AP2);
    p = parse(&f);
    CHECK_EQ(p.dir, DOT11_DIR_DIRECT);
    CHECK(memcmp(p.client, FB_STA1, 6) == 0);
    CHECK(memcmp(p.bssid, FB_AP2, 6) == 0);

    // Both bits: WDS, four addresses, no client.
    fb_header(&f, FB_FC(2, 8), 0x03, FB_AP1, FB_AP2, FB_STA1);
    fb_bytes(&f, FB_STA2, 6);
    p = parse(&f);
    CHECK_EQ(p.dir, DOT11_DIR_WDS);
    CHECK(!p.has_client && !p.has_bssid);

    // Null and QoS Null.
    fb_header(&f, FB_FC(2, 4), 0x11, FB_AP1, FB_STA1, FB_AP1);
    p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_NULL);
    fb_header(&f, FB_FC(2, 12), 0x01, FB_AP1, FB_STA1, FB_AP1);
    p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_NULL);

    // Protected data still yields addresses.
    fb_header(&f, FB_FC(2, 8), 0x41, FB_AP1, FB_STA2, FB_BCAST);
    p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_DATA);
    CHECK(memcmp(p.client, FB_STA2, 6) == 0);
}

static void test_htc_and_header_edges(void)
{
    // Order bit on a management frame: 4-byte HT Control before the body.
    fb_t f;
    fb_header(&f, FB_FC(0, 8), 0x80, FB_BCAST, FB_AP1, FB_AP1);
    fb_bytes(&f, "\xAA\xBB\xCC\xDD", 4);
    for (int i = 0; i < 8; i++) {
        fb_u8(&f, 0);
    }
    fb_le16(&f, 200);
    fb_le16(&f, 0x0001);
    fb_ssid(&f, "ExampleNet");
    dot11_frame_t p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_BEACON);
    CHECK_EQ(p.beacon_interval_tu, 200);
    CHECK_EQ(p.ies.ssid_len, 10);

    // Too short for a header, wrong protocol version, control frame.
    fb_header(&f, FB_FC(0, 8), 0, FB_BCAST, FB_AP1, FB_AP1);
    f.len = 23;
    p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_INVALID);
    CHECK(!dot11_parse(NULL, 0, &p));
    fb_header(&f, (uint8_t)(FB_FC(0, 8) | 0x01), 0, FB_BCAST, FB_AP1, FB_AP1);
    CHECK(!dot11_parse(f.buf, f.len, &p));
    fb_header(&f, FB_FC(1, 11), 0, FB_BCAST, FB_AP1, FB_AP1);
    p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_OTHER);

    // Beacon whose fixed fields are cut short.
    fb_beacon(&f, 8, FB_AP1, 0x0001);
    f.len -= 3;
    p = parse(&f);
    CHECK_EQ(p.kind, DOT11_KIND_BEACON);
    CHECK(p.truncated);
    CHECK(!p.fixed_fields);
    CHECK(!p.ies.ssid_present);

    // Auth too short.
    fb_header(&f, FB_FC(0, 11), 0, FB_AP1, FB_STA1, FB_AP1);
    fb_u8(&f, 0);
    p = parse(&f);
    CHECK(p.truncated);

    // Management header with Order bit but no room for HT Control.
    fb_header(&f, FB_FC(0, 8), 0x80, FB_BCAST, FB_AP1, FB_AP1);
    fb_u8(&f, 0);
    p = parse(&f);
    CHECK(p.truncated);
}

static void test_odd_elements(void)
{
    fb_t f;
    fb_beacon(&f, 8, FB_AP1, 0x0001);
    fb_ie(&f, 3, "", 0);           // empty DS Parameter Set
    fb_ie(&f, 11, "\x01\x00", 2);  // BSS Load too short
    fb_ie(&f, 61, "\x06", 1);      // HT Operation too short
    fb_ie(&f, 192, "\x01", 1);     // VHT Operation too short
    fb_ie(&f, 221, "\x00\x50", 2); // vendor too short
    fb_ie(&f, 221, "\x00\x50\xF2\x01\x01", 5);  // WPA element too short for a version
    fb_u8(&f, 255);                // extension with no ID
    fb_u8(&f, 0);
    fb_ie(&f, 200, "\x01\x02\x03", 3);  // unknown element
    fb_ssid(&f, "ExampleNet");
    static const uint8_t many_rates[20] = {0x82};
    fb_ie(&f, 1, many_rates, sizeof(many_rates));
    static const uint8_t many_ext[40] = {0x30};
    fb_ie(&f, 50, many_ext, sizeof(many_ext));
    dot11_frame_t p = parse(&f);
    CHECK(!p.truncated);
    CHECK_EQ(p.ies.ds_channel, 0);
    CHECK(!p.ies.bss_load);
    CHECK_EQ(p.ies.ht_primary_channel, 0);
    CHECK_EQ(p.ies.width, DOT11_WIDTH_UNKNOWN);
    CHECK(!p.ies.wpa && !p.ies.wps);
    CHECK_EQ(p.security, DOT11_SEC_OPEN);
    CHECK_EQ(p.ies.ssid_len, 10);
    CHECK_EQ(p.ies.rates_len, DOT11_RATES_MAX);
    CHECK_EQ(p.ies.ext_rates_len, DOT11_EXT_RATES_MAX);
}

// Every prefix of a full beacon: elements before the cut are kept, the
// parser never reads past the end, and a cut inside an element is flagged.
static void test_truncation(void)
{
    fb_t full;
    fb_full_beacon(&full);
    for (size_t n = 0; n <= full.len; n++) {
        fb_t f = full;
        f.len = n;
        dot11_frame_t p = parse(&f);
        if (n < 24) {
            CHECK_EQ(p.kind, DOT11_KIND_INVALID);
            continue;
        }
        CHECK_EQ(p.kind, DOT11_KIND_BEACON);
        CHECK(p.fixed_fields == (n >= 24 + 12));
        if (n >= 24 + 12 + 12) {
            CHECK(p.ies.ssid_present && p.ies.ssid_len == 10);  // SSID element is complete
        }
        if (n == full.len) {
            CHECK(!p.truncated);
        }
    }
    // Cut in the middle of the RSN element (2 bytes into it).
    size_t rsn_at = 0;
    for (size_t i = 36; i + 1 < full.len; i += 2u + full.buf[i + 1]) {
        if (full.buf[i] == 48) {
            rsn_at = i;
            break;
        }
    }
    CHECK(rsn_at > 0);
    fb_t f = full;
    f.len = rsn_at + 4;
    dot11_frame_t p = parse(&f);
    CHECK(p.truncated);
    CHECK(!p.ies.rsn);
    CHECK_EQ(p.ies.ds_channel, 6);
}

// Deterministic garbage: random bytes with a plausible header, and random
// mutations of a valid beacon. Passing means no crash and no sanitizer report.
static uint32_t s_rng = 0x12345678u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void test_fuzz(void)
{
    fb_t full;
    fb_full_beacon(&full);
    int parsed = 0;
    for (int iter = 0; iter < 200000; iter++) {
        fb_t f;
        if (iter % 2 == 0) {
            f.len = rnd() % 600;
            for (size_t i = 0; i < f.len; i++) {
                f.buf[i] = (uint8_t)rnd();
            }
            if (f.len > 0) {
                f.buf[0] &= 0xFC;  // protocol version 0 so most frames get parsed
            }
        } else {
            f = full;
            int mutations = 1 + (int)(rnd() % 8);
            for (int m = 0; m < mutations; m++) {
                f.buf[24 + rnd() % (f.len - 24)] = (uint8_t)rnd();
            }
            f.len = 24 + rnd() % (f.len - 23);
        }
        dot11_frame_t p = parse(&f);
        parsed += p.kind != DOT11_KIND_INVALID;
        CHECK(p.ies.ssid_len <= DOT11_SSID_MAX);
        CHECK(p.ies.rates_len <= DOT11_RATES_MAX);
        CHECK(p.ies.ext_rates_len <= DOT11_EXT_RATES_MAX);
        CHECK(p.ies.pairwise_count <= DOT11_SUITES_MAX);
        CHECK(p.ies.akm_count <= DOT11_SUITES_MAX);
    }
    CHECK(parsed > 100000);
}

static void test_channels(void)
{
    CHECK_EQ(dot11_channel_freq_mhz(1), 2412);
    CHECK_EQ(dot11_channel_freq_mhz(13), 2472);
    CHECK_EQ(dot11_channel_freq_mhz(14), 2484);
    CHECK_EQ(dot11_channel_freq_mhz(36), 5180);
    CHECK_EQ(dot11_channel_freq_mhz(165), 5825);
    CHECK_EQ(dot11_channel_freq_mhz(177), 5885);
    CHECK_EQ(dot11_channel_freq_mhz(0), 0);
    CHECK_EQ(dot11_channel_freq_mhz(20), 0);
    CHECK_EQ(dot11_channel_freq_mhz(178), 0);
    CHECK_EQ(dot11_channel_band(6), 1);
    CHECK_EQ(dot11_channel_band(149), 2);
    CHECK_EQ(dot11_channel_band(15), 0);
}

int main(void)
{
    test_full_beacon();
    test_probe_response();
    test_hidden_ssid();
    test_security();
    test_suite_lists();
    test_rsn_partial();
    test_widths();
    test_probe_request();
    test_assoc_auth_action();
    test_data();
    test_htc_and_header_edges();
    test_odd_elements();
    test_truncation();
    test_fuzz();
    test_channels();
    if (s_failures) {
        printf("dot11_parser: %d of %d checks FAILED\n", s_failures, s_checks);
        return 1;
    }
    printf("dot11_parser: OK (%d checks)\n", s_checks);
    return 0;
}
