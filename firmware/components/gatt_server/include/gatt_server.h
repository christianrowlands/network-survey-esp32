// NimBLE peripheral: the NS Board Service (docs/protocol.md section 2), the
// Device Information and GATT services, advertising and link setup.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    GATT_EVENT_CONNECTED,
    GATT_EVENT_DISCONNECTED,
    GATT_EVENT_ENCRYPTED,        // link encrypted (pairing done or bond restored)
    GATT_EVENT_PAIRING_CHANGED,  // pairable or bond count changed (conn_handle unused)
    GATT_EVENT_BONDS_WIPED,      // every bond was deleted (conn_handle unused)
} gatt_event_t;

// Called from the NimBLE host task. Keep it short.
typedef void (*gatt_event_cb_t)(gatt_event_t event, uint16_t conn_handle, void *ctx);

// Starts the BLE host. Advertising begins once the host has synced.
esp_err_t gatt_server_init(gatt_event_cb_t cb, void *ctx);

// "XXXX" from NS-XXXX, valid after the host has synced (empty before).
const char *gatt_server_short_id(void);

// Number of connected phones (0 to 2; 2 only during a pairing window).
int gatt_server_connection_count(void);

// Pairing (protocol.md section 3). The board is pairable while it has no bonds
// or while the explicit 60 s window is open; the window ends when a new bond
// is stored or on timeout, and then unbonded connections are dropped. These
// can be called from any task; the work runs in the NimBLE host task.

// Opens (or restarts) the window: BOOT button hold.
void gatt_server_open_pairing_window(void);

// EnterPairingMode from conn. False if conn is not encrypted with a stored
// bond (nothing happens then).
bool gatt_server_enter_pairing_mode(uint16_t conn);

// Deletes every bond and disconnects every phone after delay_ms (time for the
// ForgetBonds response to go out; 0 for the BOOT wipe).
void gatt_server_forget_bonds(uint32_t delay_ms);

// True while the board is pairable: no bonds, or the window is open.
bool gatt_server_pairable(void);

// Stored bonds.
int gatt_server_bond_count(void);

// Rebuilds the advertising data (for example after the GPS was detected).
// Callable from any task.
void gatt_server_refresh_advertising(void);

// --- Link layer interface (Control, Status, Records, Raw, OTA) ---

typedef enum {
    GATT_CHR_CONTROL,
    GATT_CHR_STATUS,
    GATT_CHR_RECORDS,
    GATT_CHR_RAW,
    GATT_CHR_OTA,
    GATT_CHR_COUNT,
} gatt_chr_t;

// Called from the NimBLE host task; keep them short (queue work elsewhere).
// write: a Control or OTA write on a bonded, encrypted link; return 0 or a
// BLE_ATT_ERR_* code for the write response.
typedef struct {
    void (*connected)(uint16_t conn);
    void (*disconnected)(uint16_t conn);
    void (*subscribed)(uint16_t conn, gatt_chr_t chr, bool enabled);
    void (*indicate_done)(uint16_t conn, gatt_chr_t chr, int status);
    int (*write)(uint16_t conn, gatt_chr_t chr, const uint8_t *data, uint16_t len);
} gatt_link_hooks_t;

// Set before gatt_server_init. hooks must stay valid.
void gatt_server_set_link_hooks(const gatt_link_hooks_t *hooks);

// Sends one notification (Status, Records, Raw, OTA) or indication (Control).
// Returns 0, BLE_HS_ENOMEM when the stack is out of buffers (try again
// later), or another BLE_HS_* error. Callable from any task.
int gatt_server_notify(uint16_t conn, gatt_chr_t chr, const uint8_t *data, uint16_t len);
int gatt_server_indicate(uint16_t conn, gatt_chr_t chr, const uint8_t *data, uint16_t len);

// Negotiated ATT MTU of a connection (23 until the exchange).
uint16_t gatt_server_mtu(uint16_t conn);

// The link is encrypted with keys from a stored bond, as last seen by the
// NimBLE host task (safe from any task; does not read the bond store).
bool gatt_server_link_bonded(uint16_t conn);

// Free NimBLE transmit buffers (mbufs), for congestion control.
int gatt_server_tx_buffers_free(void);

// Terminates a connection.
void gatt_server_disconnect(uint16_t conn);
