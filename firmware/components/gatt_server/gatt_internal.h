// Shared between gap.c and gatt_svc.c. Not part of the component API.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gatt_server.h"
#include "host/ble_uuid.h"
#include "pairing_policy.h"

// 72AAxxxx-0D8E-4D18-8877-63C24ABE28D2, little-endian byte order.
#define NS_UUID(x)                                                                                  \
    BLE_UUID128_INIT(0xD2, 0x28, 0xBE, 0x4A, 0xC2, 0x63, 0x77, 0x88, 0x18, 0x4D, 0x8E, 0x0D,      \
                     (x) & 0xFF, ((x) >> 8) & 0xFF, 0xAA, 0x72)

extern const ble_uuid128_t gatt_svc_uuid;

// Value handles of the notify/indicate characteristics, by gatt_chr_t.
extern uint16_t gatt_chr_handles[GATT_CHR_COUNT];

// Link layer callbacks (NULL until set).
extern const gatt_link_hooks_t *gatt_link_hooks;

// gatt_chr_t of a value handle, or GATT_CHR_COUNT if it is none of ours.
gatt_chr_t gatt_chr_from_handle(uint16_t attr_handle);

int gatt_svc_register(void);

// Link is encrypted with keys from a stored bond (protocol.md "Encrypted,
// bonded"): the bond must still be in the store.
bool gatt_link_bonded(uint16_t conn_handle);

// gap.c, host task only. gap_advertise applies the pairing policy's
// advertising mode shortly after (from the host queue); gap_adv_stop stops it
// at once.
void gap_advertise(void);
void gap_adv_stop(void);
void gap_emit(gatt_event_t event, uint16_t conn_handle);

// pairing.c: bond policy glue, host task only (pairing_init before the host
// starts, after ble_store_config_init).
void pairing_init(void);
void pairing_on_sync(void);
void pairing_on_reset(void);
void pairing_on_connect(uint16_t conn);
void pairing_on_disconnect(uint16_t conn);
void pairing_on_enc_change(uint16_t conn, int status);
int pairing_on_repeat_pairing(uint16_t conn);  // BLE_GAP_REPEAT_PAIRING_*
pp_adv_t pairing_advertising(void);

// Board identity, filled by gap.c at host sync.
extern uint8_t gatt_bt_address[6];  // printed order, most significant byte first
extern char gatt_short_id[5];
