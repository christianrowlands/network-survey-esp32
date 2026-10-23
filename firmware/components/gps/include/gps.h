// Optional UART NMEA GPS receiver (board Kconfig "GPS receiver"). A task reads
// the UART, parses GGA and RMC (nmea.h) and hands GpsFix records to the sink
// about once a second while capture is active.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "ns_board.pb.h"

typedef struct {
    // A GpsFix record (board_time_ms monotonic). Called from the GPS task.
    void (*record)(const ns_board_v1_Record *rec, void *ctx);
    // gps_state() changed, or the GPS was detected (board_gps_present()).
    void (*changed)(void *ctx);
    void *ctx;
} gps_sink_t;

// Starts the GPS task when the board config has a GPS (fitted or detect);
// otherwise does nothing and returns ESP_OK.
esp_err_t gps_init(const gps_sink_t *sink);

// Fix records go to the sink only while active (surveying).
void gps_set_active(bool active);

// NOT_PRESENT, NO_FIX, or FIX when a fix arrived in the last 3 seconds.
ns_board_v1_GpsState gps_state(void);
