// Pairing commands from the phone (EnterPairingMode, ForgetBonds), BOOT button
// actions and the pairing LED, wired to the bond policy in gatt_server.
#pragma once

#include <stdint.h>

#include "gatt_server.h"
#include "ui_io.h"

// Registers the Control command handlers. Call before gatt_server_init.
void pairing_cmds_register(void);

void pairing_cmds_on_button(ui_button_event_t event);

// GATT_EVENT_PAIRING_CHANGED and GATT_EVENT_BONDS_WIPED.
void pairing_cmds_on_gatt(gatt_event_t event);
