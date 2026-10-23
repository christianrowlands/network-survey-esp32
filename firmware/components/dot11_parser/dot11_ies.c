// Information element parsing for beacons, probe responses and requests.

#include <string.h>

#include "dot11_parser.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Element IDs
#define EID_SSID 0
#define EID_RATES 1
#define EID_DS_PARAMS 3
#define EID_BSS_LOAD 11
#define EID_HT_CAP 45
#define EID_RSN 48
#define EID_EXT_RATES 50
#define EID_HT_OP 61
#define EID_INTERWORKING 107
#define EID_VHT_CAP 191
#define EID_VHT_OP 192
#define EID_VENDOR 221
#define EID_EXTENSION 255

// Element ID extensions
#define EXT_HE_CAP 35
#define EXT_EHT_OP 106
#define EXT_EHT_CAP 108

static const uint8_t OUI_MS[3] = {0x00, 0x50, 0xF2};    // WPA and WPS vendor elements
static const uint8_t OUI_WFA[3] = {0x50, 0x6F, 0x9A};   // Wi-Fi Alliance (Hotspot 2.0)
#define MS_TYPE_WPA 1
#define MS_TYPE_WPS 4
#define WFA_TYPE_HS20 0x10

static uint32_t suite(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void add_suite(uint32_t *list, uint8_t *count, uint32_t value)
{
    for (uint8_t i = 0; i < *count; i++) {
        if (list[i] == value) {
            return;
        }
    }
    if (*count < DOT11_SUITES_MAX) {
        list[(*count)++] = value;
    }
}

// Parsed RSN or WPA element body. Trailing fields may be omitted by the
// sender, so each one is optional.
typedef struct {
    uint32_t group;
    uint8_t pairwise_count;
    uint32_t pairwise[DOT11_SUITES_MAX];
    uint8_t akm_count;
    uint32_t akm[DOT11_SUITES_MAX];
    uint8_t akm_kinds;  // DOT11_AKM_* over every listed AKM, not just the kept ones
} suites_t;

static uint8_t akm_kind(uint32_t s)
{
    if ((s >> 8) != 0x000FAC) {
        return DOT11_AKM_OTHER;
    }
    switch (s & 0xFF) {
    case 8: case 9: case 12: case 13: case 24: case 25:
        return DOT11_AKM_SAE;
    case 18:
        return DOT11_AKM_OWE;
    case 1: case 2: case 3: case 4: case 5: case 6:
        return DOT11_AKM_LEGACY;
    default:
        return DOT11_AKM_OTHER;
    }
}

// body: version (2), group cipher (4), pairwise count (2) + suites,
// AKM count (2) + suites, then fields we do not need. Keeps whatever fits:
// a list cut short by the element length ends parsing.
static void parse_suites(const uint8_t *body, size_t len, suites_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t i = 2;
    if (len < i + 4) {
        return;
    }
    out->group = suite(body + i);
    i += 4;
    for (int list = 0; list < 2; list++) {
        if (len - i < 2) {
            return;
        }
        uint16_t n = le16(body + i);
        i += 2;
        for (uint16_t k = 0; k < n; k++, i += 4) {
            if (len - i < 4) {
                return;  // the count promised more suites than the element holds
            }
            if (list == 0) {
                add_suite(out->pairwise, &out->pairwise_count, suite(body + i));
            } else {
                add_suite(out->akm, &out->akm_count, suite(body + i));
                out->akm_kinds |= akm_kind(suite(body + i));
            }
        }
    }
}

static void merge_suites(dot11_ies_t *ies, const suites_t *s)
{
    if (ies->group_cipher == 0) {
        ies->group_cipher = s->group;
    }
    for (uint8_t k = 0; k < s->pairwise_count; k++) {
        add_suite(ies->pairwise, &ies->pairwise_count, s->pairwise[k]);
    }
    for (uint8_t k = 0; k < s->akm_count; k++) {
        add_suite(ies->akm, &ies->akm_count, s->akm[k]);
    }
}

static dot11_width_t vht_width(const uint8_t *body, uint8_t len)
{
    if (len < 3) {
        return DOT11_WIDTH_UNKNOWN;
    }
    uint8_t ccfs0 = body[1];
    uint8_t ccfs1 = body[2];
    switch (body[0]) {
    case 1:
        // 802.11-2016 signaling: CCFS1 set means 160 (8 apart) or 80+80.
        if (ccfs1 == 0) {
            return DOT11_WIDTH_80;
        }
        return (ccfs1 > ccfs0 ? ccfs1 - ccfs0 : ccfs0 - ccfs1) == 8 ? DOT11_WIDTH_160 : DOT11_WIDTH_80P80;
    case 2:
        return DOT11_WIDTH_160;
    case 3:
        return DOT11_WIDTH_80P80;
    default:
        return DOT11_WIDTH_UNKNOWN;  // 20 or 40: HT Operation decides
    }
}

// EHT Operation: parameters (1), basic EHT-MCS and NSS set (4), then the
// optional operation information: control (1, bits 0-2 width), CCFS0, CCFS1.
static dot11_width_t eht_width(const uint8_t *body, uint8_t len)
{
    if (len < 6 || !(body[0] & 0x01)) {
        return DOT11_WIDTH_UNKNOWN;
    }
    static const dot11_width_t map[] = {DOT11_WIDTH_20, DOT11_WIDTH_40, DOT11_WIDTH_80,
                                        DOT11_WIDTH_160, DOT11_WIDTH_320};
    uint8_t w = body[5] & 0x07;
    return w < sizeof(map) / sizeof(map[0]) ? map[w] : DOT11_WIDTH_UNKNOWN;
}

static void parse_vendor(const uint8_t *body, uint8_t len, dot11_ies_t *ies, suites_t *wpa)
{
    if (len < 4) {
        return;
    }
    if (memcmp(body, OUI_MS, 3) == 0) {
        if (body[3] == MS_TYPE_WPA && !ies->wpa && len >= 6) {
            // WPA element: same layout as RSN after the 4-byte OUI and type.
            ies->wpa = true;
            parse_suites(body + 4, len - 4u, wpa);
        } else if (body[3] == MS_TYPE_WPS) {
            ies->wps = true;
        }
    } else if (memcmp(body, OUI_WFA, 3) == 0 && body[3] == WFA_TYPE_HS20) {
        ies->passpoint = true;
    }
}

bool dot11_parse_ies(const uint8_t *p, size_t len, dot11_ies_t *ies)
{
    memset(ies, 0, sizeof(*ies));
    suites_t rsn = {0};
    suites_t wpa = {0};
    dot11_width_t ht_w = DOT11_WIDTH_UNKNOWN;
    dot11_width_t vht_w = DOT11_WIDTH_UNKNOWN;
    dot11_width_t eht_w = DOT11_WIDTH_UNKNOWN;
    bool complete = true;
    size_t i = 0;

    while (i < len) {
        if (len - i < 2 || len - i - 2 < p[i + 1]) {
            complete = false;
            break;
        }
        uint8_t id = p[i];
        uint8_t n = p[i + 1];
        const uint8_t *b = p + i + 2;
        i += 2u + n;

        switch (id) {
        case EID_SSID:
            // Only the first SSID element names the network; over-long ones are malformed.
            if (!ies->ssid_present && n <= DOT11_SSID_MAX) {
                ies->ssid_present = true;
                ies->ssid_len = n;
                memcpy(ies->ssid, b, n);
                bool zero = true;
                for (uint8_t k = 0; k < n; k++) {
                    zero = zero && b[k] == 0;
                }
                ies->ssid_hidden = zero;
            }
            break;
        case EID_RATES:
            if (ies->rates_len == 0) {
                ies->rates_len = n < DOT11_RATES_MAX ? n : DOT11_RATES_MAX;
                memcpy(ies->rates, b, ies->rates_len);
            }
            break;
        case EID_EXT_RATES:
            if (ies->ext_rates_len == 0) {
                ies->ext_rates_len = n < DOT11_EXT_RATES_MAX ? n : DOT11_EXT_RATES_MAX;
                memcpy(ies->ext_rates, b, ies->ext_rates_len);
            }
            break;
        case EID_DS_PARAMS:
            if (n >= 1 && ies->ds_channel == 0) {
                ies->ds_channel = b[0];
            }
            break;
        case EID_BSS_LOAD:
            if (n >= 3 && !ies->bss_load) {
                ies->bss_load = true;
                ies->station_count = le16(b);
                ies->channel_utilization = b[2];
            }
            break;
        case EID_HT_CAP:
            ies->phy_flags |= DOT11_PHY_HT;
            break;
        case EID_HT_OP:
            if (n >= 2 && ies->ht_primary_channel == 0) {
                ies->ht_primary_channel = b[0];
                uint8_t offset = b[1] & 0x03;  // 1 above, 3 below, 0 none
                ht_w = (offset == 1 || offset == 3) ? DOT11_WIDTH_40 : DOT11_WIDTH_20;
            }
            break;
        case EID_RSN:
            if (!ies->rsn && n >= 2) {
                ies->rsn = true;
                parse_suites(b, n, &rsn);
            }
            break;
        case EID_INTERWORKING:
            ies->interworking = true;
            break;
        case EID_VHT_CAP:
            ies->phy_flags |= DOT11_PHY_VHT;
            break;
        case EID_VHT_OP:
            vht_w = vht_width(b, n);
            break;
        case EID_VENDOR:
            parse_vendor(b, n, ies, &wpa);
            break;
        case EID_EXTENSION:
            if (n >= 1) {
                if (b[0] == EXT_HE_CAP) {
                    ies->phy_flags |= DOT11_PHY_HE;
                } else if (b[0] == EXT_EHT_CAP) {
                    ies->phy_flags |= DOT11_PHY_EHT;
                } else if (b[0] == EXT_EHT_OP) {
                    eht_w = eht_width(b + 1, (uint8_t)(n - 1));
                }
            }
            break;
        default:
            break;
        }
    }

    // RSN suites first, then WPA, so the RSN group cipher wins.
    if (ies->rsn) {
        merge_suites(ies, &rsn);
        ies->rsn_akm_kinds = rsn.akm_kinds;
    }
    if (ies->wpa) {
        merge_suites(ies, &wpa);
    }
    ies->width = eht_w != DOT11_WIDTH_UNKNOWN   ? eht_w
                 : vht_w != DOT11_WIDTH_UNKNOWN ? vht_w
                                                : ht_w;
    return complete;
}

dot11_security_t dot11_derive_security(bool privacy, const dot11_ies_t *ies)
{
    if (!ies->rsn && !ies->wpa) {
        return privacy ? DOT11_SEC_WEP : DOT11_SEC_OPEN;
    }
    if (!ies->rsn) {
        return DOT11_SEC_WPA;
    }
    uint8_t k = ies->rsn_akm_kinds;
    if (k == DOT11_AKM_OWE) {
        return DOT11_SEC_OWE;
    }
    if (k & DOT11_AKM_SAE) {
        return (k & DOT11_AKM_LEGACY) ? DOT11_SEC_WPA2_WPA3 : DOT11_SEC_WPA3;
    }
    return ies->wpa ? DOT11_SEC_WPA_WPA2 : DOT11_SEC_WPA2;
}
