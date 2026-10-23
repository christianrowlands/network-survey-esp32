// Which connections gap.c has reported to the rest of the firmware. NimBLE
// reports BLE_GAP_EVENT_CONNECT for a peripheral connection only after it has
// read the phone's version and features, and a phone that reconnects with a
// bond often encrypts first, so ENC_CHANGE and the SUBSCRIBE events of its
// restored CCCDs can come before CONNECT. gap.c reports a connection at its
// first event of any of these kinds, so the pairing policy and the link layer
// always see the connection before its encryption and subscriptions. Pure C
// (no ESP-IDF) for the host tests.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CT_MAX_CONNS 4

typedef struct {
    int count;
    uint16_t handles[CT_MAX_CONNS];
} ct_state_t;

void ct_init(ct_state_t *t);

// An event for handle (CONNECT, ENC_CHANGE, SUBSCRIBE, ...): true the first
// time, when the caller must report the connection before handling the event.
bool ct_seen(ct_state_t *t, uint16_t handle);

// handle disconnected: true if it had been reported (then report the
// disconnection too).
bool ct_gone(ct_state_t *t, uint16_t handle);
