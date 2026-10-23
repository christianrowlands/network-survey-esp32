// Board description: model, chip, radio capabilities and optional hardware,
// from the board selected in Kconfig (boards/<board>.defaults).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ns_board.pb.h"

ns_board_v1_BoardModel board_model(void);
const char *board_model_name(void);
ns_board_v1_Chip board_chip(void);
const char *board_chip_name(void);

// True when the radio can receive on 5 GHz (ESP32-C5).
bool board_has_5ghz(void);
// A GPS receiver is fitted, or was detected (gps.c sets it).
bool board_gps_present(void);
void board_set_gps_present(bool present);

bool board_has_battery(void);
// Reads the battery voltage. ESP_ERR_NOT_SUPPORTED when the board has no
// battery input.
esp_err_t board_battery_mv(uint32_t *out_mv);
