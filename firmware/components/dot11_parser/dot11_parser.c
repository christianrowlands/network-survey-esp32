#include "dot11_parser.h"

#include <string.h>

// Frame Control
#define FC_TYPE_MGMT 0
#define FC_TYPE_DATA 2
#define FC_FLAG_TO_DS 0x01
#define FC_FLAG_FROM_DS 0x02
#define FC_FLAG_ORDER 0x80  // +HTC: a 4-byte HT Control field follows the header

// Management subtypes
#define ST_ASSOC_REQ 0
#define ST_REASSOC_REQ 2
#define ST_PROBE_REQ 4
#define ST_PROBE_RESP 5
#define ST_BEACON 8
#define ST_AUTH 11
#define ST_ACTION 13
#define ST_ACTION_NOACK 14

#define MGMT_HDR_LEN 24
#define CAP_PRIVACY 0x0010

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Fixed field length before the elements of a management frame body, or -1
// if the subtype carries no elements we parse.
static int mgmt_fixed_len(uint8_t subtype)
{
    switch (subtype) {
    case ST_BEACON:
    case ST_PROBE_RESP:
        return 12;  // timestamp (8), beacon interval (2), capability (2)
    case ST_PROBE_REQ:
        return 0;
    case ST_ASSOC_REQ:
        return 4;   // capability (2), listen interval (2)
    case ST_REASSOC_REQ:
        return 10;  // capability, listen interval, current AP address (6)
    default:
        return -1;
    }
}

static void parse_mgmt(const uint8_t *body, size_t len, dot11_frame_t *f)
{
    switch (f->subtype) {
    case ST_BEACON: f->kind = DOT11_KIND_BEACON; break;
    case ST_PROBE_RESP: f->kind = DOT11_KIND_PROBE_RESP; break;
    case ST_PROBE_REQ: f->kind = DOT11_KIND_PROBE_REQ; break;
    case ST_ASSOC_REQ: f->kind = DOT11_KIND_ASSOC_REQ; break;
    case ST_REASSOC_REQ: f->kind = DOT11_KIND_REASSOC_REQ; break;
    case ST_AUTH: f->kind = DOT11_KIND_AUTH; break;
    case ST_ACTION:
    case ST_ACTION_NOACK: f->kind = DOT11_KIND_ACTION; break;
    default: f->kind = DOT11_KIND_OTHER; return;
    }

    if (f->kind == DOT11_KIND_AUTH) {
        if (len >= 4) {
            f->auth_algorithm = le16(body);
            f->auth_seq = le16(body + 2);
            f->fixed_fields = true;
        } else {
            f->truncated = true;
        }
        return;
    }
    int fixed = mgmt_fixed_len(f->subtype);
    if (fixed < 0) {
        return;
    }
    if (len < (size_t)fixed) {
        f->truncated = true;
        return;
    }
    f->fixed_fields = true;
    if (f->kind == DOT11_KIND_BEACON || f->kind == DOT11_KIND_PROBE_RESP) {
        f->beacon_interval_tu = le16(body + 8);
        f->capability = le16(body + 10);
    } else if (f->kind == DOT11_KIND_ASSOC_REQ || f->kind == DOT11_KIND_REASSOC_REQ) {
        f->capability = le16(body);
        if (f->kind == DOT11_KIND_REASSOC_REQ) {
            memcpy(f->current_ap, body + 4, DOT11_MAC_LEN);
        }
    }
    if (!dot11_parse_ies(body + fixed, len - (size_t)fixed, &f->ies)) {
        f->truncated = true;
    }
    if (f->kind == DOT11_KIND_BEACON || f->kind == DOT11_KIND_PROBE_RESP) {
        f->security = dot11_derive_security((f->capability & CAP_PRIVACY) != 0, &f->ies);
    }
}

static void parse_data(dot11_frame_t *f)
{
    f->kind = (f->subtype & 0x04) ? DOT11_KIND_NULL : DOT11_KIND_DATA;
    switch (f->flags & (FC_FLAG_TO_DS | FC_FLAG_FROM_DS)) {
    case FC_FLAG_TO_DS:  // addr1 BSSID, addr2 station (transmitter)
        f->dir = DOT11_DIR_TO_AP;
        memcpy(f->client, f->addr2, DOT11_MAC_LEN);
        memcpy(f->bssid, f->addr1, DOT11_MAC_LEN);
        break;
    case FC_FLAG_FROM_DS:  // addr1 station (receiver), addr2 BSSID
        f->dir = DOT11_DIR_FROM_AP;
        memcpy(f->client, f->addr1, DOT11_MAC_LEN);
        memcpy(f->bssid, f->addr2, DOT11_MAC_LEN);
        break;
    case 0:  // addr2 transmitter, addr3 BSSID
        f->dir = DOT11_DIR_DIRECT;
        memcpy(f->client, f->addr2, DOT11_MAC_LEN);
        memcpy(f->bssid, f->addr3, DOT11_MAC_LEN);
        break;
    default:
        f->dir = DOT11_DIR_WDS;  // four addresses, no single client or BSS
        return;
    }
    f->has_client = !dot11_is_group(f->client);
    f->has_bssid = !dot11_is_group(f->bssid);
}

bool dot11_parse(const uint8_t *buf, size_t len, dot11_frame_t *out)
{
    memset(out, 0, sizeof(*out));
    // Data and management frames have at least three addresses (24 bytes).
    if (buf == NULL || len < MGMT_HDR_LEN || (buf[0] & 0x03) != 0) {
        out->kind = DOT11_KIND_INVALID;
        return false;
    }
    out->type = (buf[0] >> 2) & 0x03;
    out->subtype = (buf[0] >> 4) & 0x0F;
    out->flags = buf[1];
    memcpy(out->addr1, buf + 4, DOT11_MAC_LEN);
    memcpy(out->addr2, buf + 10, DOT11_MAC_LEN);
    memcpy(out->addr3, buf + 16, DOT11_MAC_LEN);

    if (out->type == FC_TYPE_MGMT) {
        size_t hdr = MGMT_HDR_LEN + ((out->flags & FC_FLAG_ORDER) ? 4u : 0u);
        if (len < hdr) {
            out->kind = DOT11_KIND_OTHER;
            out->truncated = true;
            return true;
        }
        parse_mgmt(buf + hdr, len - hdr, out);
    } else if (out->type == FC_TYPE_DATA) {
        parse_data(out);
    } else {
        out->kind = DOT11_KIND_OTHER;
    }
    return true;
}

uint32_t dot11_channel_freq_mhz(uint32_t channel)
{
    if (channel >= 1 && channel <= 13) {
        return 2407 + 5 * channel;
    }
    if (channel == 14) {
        return 2484;
    }
    if (channel >= 32 && channel <= 177) {
        return 5000 + 5 * channel;
    }
    return 0;
}

uint32_t dot11_channel_band(uint32_t channel)
{
    if (channel >= 1 && channel <= 14) {
        return 1;
    }
    if (channel >= 32 && channel <= 177) {
        return 2;
    }
    return 0;
}
