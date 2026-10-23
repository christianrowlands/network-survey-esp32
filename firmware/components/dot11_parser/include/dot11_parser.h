// 802.11 management and data frame parser.
//
// Pure C99 with no ESP-IDF dependencies so it builds and runs in host tests.
// Input is one received frame starting at Frame Control, without the FCS. The
// buffer may be cut short (the sniffer copies a capped prefix): every field
// and information element is bounds checked, and an element that runs past
// the end stops element parsing and sets `truncated`.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOT11_MAC_LEN 6
#define DOT11_SSID_MAX 32
#define DOT11_RATES_MAX 16      // Supported Rates octets kept (ApObservation limit)
#define DOT11_EXT_RATES_MAX 32  // Extended Supported Rates octets kept
#define DOT11_SUITES_MAX 8      // Pairwise ciphers and AKM suites kept

// Frame classes the parser understands. Anything else is DOT11_KIND_OTHER.
typedef enum {
    DOT11_KIND_INVALID = 0,  // Too short or not protocol version 0
    DOT11_KIND_OTHER,        // Valid header, not a frame we summarize
    DOT11_KIND_BEACON,
    DOT11_KIND_PROBE_RESP,
    DOT11_KIND_PROBE_REQ,
    DOT11_KIND_ASSOC_REQ,
    DOT11_KIND_REASSOC_REQ,
    DOT11_KIND_AUTH,
    DOT11_KIND_ACTION,  // Action and Action No Ack
    DOT11_KIND_DATA,    // Data and QoS Data
    DOT11_KIND_NULL,    // Null and QoS Null (no payload)
} dot11_kind_t;

// Data frame direction from the To DS and From DS bits.
typedef enum {
    DOT11_DIR_NONE = 0,  // Not a data frame
    DOT11_DIR_TO_AP,     // To DS: station to AP
    DOT11_DIR_FROM_AP,   // From DS: AP to station
    DOT11_DIR_DIRECT,    // Neither: IBSS or direct link
    DOT11_DIR_WDS,       // Both: mesh or WDS bridge
} dot11_dir_t;

typedef enum {
    DOT11_SEC_UNKNOWN = 0,
    DOT11_SEC_OPEN,
    DOT11_SEC_WEP,
    DOT11_SEC_WPA,
    DOT11_SEC_WPA_WPA2,
    DOT11_SEC_WPA2,
    DOT11_SEC_WPA3,
    DOT11_SEC_WPA2_WPA3,
    DOT11_SEC_OWE,
} dot11_security_t;  // Same values as the protocol's SecurityType

typedef enum {
    DOT11_WIDTH_UNKNOWN = 0,
    DOT11_WIDTH_20,
    DOT11_WIDTH_40,
    DOT11_WIDTH_80,
    DOT11_WIDTH_160,
    DOT11_WIDTH_80P80,
    DOT11_WIDTH_320,
} dot11_width_t;  // Same values as the protocol's ChannelWidth

// Capability element bits, same values as the protocol's PhyFlag.
#define DOT11_PHY_HT 0x1u
#define DOT11_PHY_VHT 0x2u
#define DOT11_PHY_HE 0x4u
#define DOT11_PHY_EHT 0x8u

// RSN AKM classes for the security summary (docs/protocol.md section 8).
#define DOT11_AKM_SAE 0x1u     // 8, 9, 24, 25 and Suite B 12, 13
#define DOT11_AKM_OWE 0x2u     // 18
#define DOT11_AKM_LEGACY 0x4u  // 1 to 6
#define DOT11_AKM_OTHER 0x8u   // Anything else, including vendor OUIs

// Information elements of beacons, probe responses and requests.
typedef struct {
    bool ssid_present;
    bool ssid_hidden;  // Element empty or all zero bytes
    uint8_t ssid_len;
    uint8_t ssid[DOT11_SSID_MAX];

    uint8_t rates_len;
    uint8_t rates[DOT11_RATES_MAX];
    uint8_t ext_rates_len;
    uint8_t ext_rates[DOT11_EXT_RATES_MAX];

    uint8_t ds_channel;          // DS Parameter Set, 0 if absent
    uint8_t ht_primary_channel;  // HT Operation primary channel, 0 if absent

    bool rsn;  // RSN element present
    bool wpa;  // WPA vendor element present
    uint32_t group_cipher;  // (OUI << 8) | type; RSN first, else WPA. 0 if none
    uint8_t pairwise_count;
    uint32_t pairwise[DOT11_SUITES_MAX];  // RSN suites, then WPA suites
    uint8_t akm_count;
    uint32_t akm[DOT11_SUITES_MAX];
    uint8_t rsn_akm_kinds;  // DOT11_AKM_* bits over every RSN AKM, even past the cap

    bool wps;
    bool interworking;  // Interworking element (802.11u)
    bool passpoint;     // Hotspot 2.0 Indication vendor element
    uint8_t phy_flags;  // DOT11_PHY_* capability elements present
    dot11_width_t width;

    bool bss_load;
    uint16_t station_count;
    uint8_t channel_utilization;
} dot11_ies_t;

typedef struct {
    dot11_kind_t kind;
    uint8_t type;     // Frame Control type (0 mgmt, 1 ctrl, 2 data)
    uint8_t subtype;  // Frame Control subtype
    uint8_t flags;    // Frame Control flags byte
    bool truncated;   // The buffer ended inside a field or element, or the
                      // capture was cut (set by the caller); elements may be missing
    bool fixed_fields;  // The body's fixed fields were read (beacon interval,
                        // capability, auth fields, current AP)

    uint8_t addr1[DOT11_MAC_LEN];
    uint8_t addr2[DOT11_MAC_LEN];
    uint8_t addr3[DOT11_MAC_LEN];

    // Data frames: who is the station and which BSS it talks to.
    dot11_dir_t dir;
    bool has_client;
    uint8_t client[DOT11_MAC_LEN];
    bool has_bssid;
    uint8_t bssid[DOT11_MAC_LEN];

    // Beacon and probe response fixed fields.
    uint16_t beacon_interval_tu;
    uint16_t capability;  // Also set for association and reassociation requests

    // Authentication fixed fields.
    uint16_t auth_algorithm;
    uint16_t auth_seq;

    // Reassociation request: the AP the station is moving from.
    uint8_t current_ap[DOT11_MAC_LEN];

    dot11_ies_t ies;  // Beacons, probe responses, probe/assoc/reassoc requests
    dot11_security_t security;  // Beacons and probe responses
} dot11_frame_t;

// Parses one frame. Returns false (kind INVALID) for frames too short for a
// header or with a non-zero protocol version. Never reads past len.
bool dot11_parse(const uint8_t *buf, size_t len, dot11_frame_t *out);

// Parses a run of information elements into out (cleared first). Returns
// false if an element ran past len; everything before it is kept.
bool dot11_parse_ies(const uint8_t *p, size_t len, dot11_ies_t *out);

// True for group (multicast or broadcast) addresses.
static inline bool dot11_is_group(const uint8_t *mac) { return (mac[0] & 0x01) != 0; }

// True for locally administered addresses (often randomized client MACs).
static inline bool dot11_is_local(const uint8_t *mac) { return (mac[0] & 0x02) != 0; }

// Security summary per docs/protocol.md section 8, from the privacy bit and
// the parsed RSN and WPA elements.
dot11_security_t dot11_derive_security(bool privacy, const dot11_ies_t *ies);

// Centre frequency of a 20 MHz channel: 2.4 GHz for 1 to 14, 5 GHz for 32 to
// 177. Returns 0 for any other number.
uint32_t dot11_channel_freq_mhz(uint32_t channel);

// 1 for 2.4 GHz, 2 for 5 GHz (the protocol's Band values), 0 if invalid.
uint32_t dot11_channel_band(uint32_t channel);

#ifdef __cplusplus
}
#endif
