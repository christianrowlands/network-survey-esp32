// Shared between the files of the link component. Not part of its API.
//   link.c          the link task (sending, per-connection state)
//   link_gatt.c     GATT callbacks, event queue, Control request slots
//   link_sink.c     clock, record and raw rings, the sinks
//   link_control.c  Control commands and survey state
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "link.h"
#include "ns_board.pb.h"

// --- link_control.c ---

void link_control_init(const link_survey_ops_t *ops, link_status_hook_t hook);

// Decodes and runs one ControlRequest written by conn. hello_ok is the
// connection's "compatible Hello answered" flag (read and updated). Always
// fills resp, including the request_id. Runs in the link task.
void link_control_handle(uint16_t conn, bool *hello_ok, const uint8_t *data, size_t len,
                         ns_board_v1_ControlResponse *resp);

// Fills time, state, role, uptime, heap, battery and bond fields, then the
// status hook's fields.
void link_control_fill_status(ns_board_v1_BoardStatus *status);

ns_board_v1_Role link_control_role(void);

// Called once a second from the link task: a board with no bonded phone for
// 5 minutes stops surveying (protocol.md section 5).
void link_control_tick(bool bonded_phone_connected);

// Configure.raw_frames is on.
bool link_control_raw_enabled(void);

// --- link_sink.c ---

esp_err_t link_sink_init(void);

void link_clock_set_unix(uint64_t unix_ms);

// MTU used to size records and raw frames as they are queued.
void link_sink_set_fit_mtu(uint16_t mtu);

// Build one Records frame (a RecordBatch of records with one time base) or
// one Raw frame (RawFrame messages) from the rings into buf, sequence 0.
// Returns the frame length (0: nothing queued) and the message count.
size_t link_sink_build_records(uint8_t *buf, size_t payload, int *count);
size_t link_sink_build_raw(uint8_t *buf, size_t payload, int *count);

bool link_sink_records_empty(void);
void link_sink_clear_raw(void);

// Whether a connection past Hello is subscribed to Raw (link_raw_wanted).
void link_sink_set_raw_listener(bool listening);

// Counts records or raw frames lost after they left the ring.
void link_sink_count_lost(bool raw, uint32_t n);

// Ring fill and drop counters for BoardStatus.
void link_sink_fill_status(ns_board_v1_BoardStatus *status);

// Records submitted since the last call.
uint32_t link_sink_take_produced(void);

// --- link_gatt.c ---

#define LINK_CTL_MAX 512  // largest ControlRequest write

typedef enum {
    LINK_EV_CONNECTED,
    LINK_EV_DISCONNECTED,
    LINK_EV_SUBSCRIBED,
    LINK_EV_INDICATE_DONE,
    LINK_EV_OTA_ACK,
    LINK_EV_WAKE,
} link_event_type_t;

typedef struct {
    uint8_t type;
    uint8_t chr;
    bool enabled;
    uint8_t len;
    uint16_t conn;
    int16_t status;
    uint8_t data[ns_board_v1_OtaAck_size];
} link_event_t;

esp_err_t link_gatt_init(void);

// Next event from the GATT callbacks, waiting up to wait ticks.
bool link_gatt_next_event(link_event_t *ev, TickType_t wait);

// Oldest Control request written by conn: copies it to out (LINK_CTL_MAX
// bytes) and frees its slot. False when there is none.
bool link_gatt_take_request(uint16_t conn, uint8_t *out, size_t *len);
