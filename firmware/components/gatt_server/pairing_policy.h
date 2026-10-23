// Pairing and bond policy (docs/protocol.md section 3) as pure logic, with no
// ESP-IDF or NimBLE includes so host tests can drive every transition. The
// NimBLE glue (pairing.c) feeds it events from the host task and carries out
// what it returns: drop connections, gate pairing requests, advertise.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PP_WINDOW_MS 60000u
// A window that times out while a phone without a bond is connected (it may be
// in the middle of pairing) gets this much more, once. 30 s is the SMP timeout.
#define PP_WINDOW_EXTEND_MS 30000u
// A connection without a bond is dropped after this long, so strangers cannot
// hold the connection slots: 30 s while the board is not pairable, 90 s while
// it is (time for the user to answer the pairing prompt).
#define PP_UNBONDED_GRACE_MS 30000u
#define PP_PAIRABLE_GRACE_MS 90000u
#define PP_MAX_CONNS 4

// Manufacturer data flag bits owned by the policy (protocol.md section 1).
#define PP_ADV_FLAG_PAIRABLE 0x01
#define PP_ADV_FLAG_HAS_BOND 0x02

typedef struct {
    uint16_t handle;
    bool bonded;  // encrypted with keys from a stored bond
    uint64_t since_ms;
} pp_conn_t;

typedef struct {
    int max_bonds;
    int max_conns;  // what the stack allows; more than 1 only during a window
    int bonds;
    bool window;  // explicit pairing window open
    bool window_extended;
    uint64_t window_end_ms;
    int conn_count;
    pp_conn_t conns[PP_MAX_CONNS];
} pp_state_t;

// What the caller must do after an event.
typedef struct {
    bool changed;  // pairable or bond count changed: refresh advertising, LED, Status
    int drop_count;
    uint16_t drop[PP_MAX_CONNS];  // connections to terminate
} pp_out_t;

typedef enum {
    PP_PAIR_DENY,     // reject the pairing request, keep the stored bonds
    PP_PAIR_ALLOW,    // pair; there is room for the new bond
    PP_PAIR_REPLACE,  // pair; the new bond replaces the oldest one
} pp_pair_t;

typedef struct {
    bool on;        // connectable advertising (else stopped)
                    // While no bonded phone is connected the board keeps
                    // advertising (up to max_conns), so a stranger's
                    // connection does not hide it from its owner.
    uint8_t flags;  // PP_ADV_FLAG_*
} pp_adv_t;

void pp_init(pp_state_t *p, int max_bonds, int max_conns, int bonds);

// No bonds yet, or an explicit window is open.
bool pp_pairable(const pp_state_t *p);

// Decision for a new pairing (a pairing request, a repeated pairing from a
// phone that lost its keys, or a bond store overflow).
pp_pair_t pp_pair_request(const pp_state_t *p);

pp_adv_t pp_advertising(const pp_state_t *p);

// Events. Each fills out (cleared first).
void pp_connected(pp_state_t *p, uint16_t handle, uint64_t now_ms, pp_out_t *out);
void pp_disconnected(pp_state_t *p, uint16_t handle, pp_out_t *out);
// Encryption is up on handle. bonded: with keys from a stored bond. new_bond:
// this encryption came from a pairing that just stored a bond. bonds: stored
// bonds now.
void pp_encrypted(pp_state_t *p, uint16_t handle, bool bonded, bool new_bond, int bonds, pp_out_t *out);
// Bond count changed outside a pairing (repeat pairing deleted one, replace).
void pp_bonds_changed(pp_state_t *p, int bonds, pp_out_t *out);
// handle no longer holds a stored bond (its bond was deleted or replaced, or
// encryption failed); bonds: stored bonds now.
void pp_link_unbonded(pp_state_t *p, uint16_t handle, int bonds, pp_out_t *out);
// handle is connected and encrypted with a stored bond.
bool pp_link_bonded(const pp_state_t *p, uint16_t handle);
// BOOT button hold: opens (or restarts) the 60 s window.
void pp_open_window(pp_state_t *p, uint64_t now_ms, pp_out_t *out);
// EnterPairingMode from handle. Only a bonded connection may open the window;
// returns false (nothing changes) otherwise.
bool pp_enter_pairing_mode(pp_state_t *p, uint16_t handle, uint64_t now_ms, pp_out_t *out);
// ForgetBonds or the BOOT wipe: every bond is gone, every phone is dropped.
void pp_bonds_wiped(pp_state_t *p, pp_out_t *out);
// About once a second: window timeout (with one extension while a phone
// without a bond is connected) and the grace drop of unbonded connections.
void pp_tick(pp_state_t *p, uint64_t now_ms, pp_out_t *out);
