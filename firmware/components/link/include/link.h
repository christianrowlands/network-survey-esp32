// Link layer: the board side of docs/protocol.md sections 4 to 9. It frames
// notifications (sequence numbers, packing to the MTU), buffers records in a
// drop-oldest ring, answers Control commands and sends Status once a second.
// Capture code hands records to link_submit_record and never waits for BLE.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ns_board.pb.h"

// Survey backend, provided by the capture modules and wired up in main.
typedef struct {
    // Validates and applies a configuration, while idle or surveying. Fills
    // result->channels and result->role (link fills raw_snaplen). Returns
    // ESP_ERR_INVALID_ARG when a Wi-Fi role has no usable channel.
    esp_err_t (*configure)(const ns_board_v1_Configure *cfg, ns_board_v1_ConfigureResult *result);
    esp_err_t (*start)(void);  // idempotent
    esp_err_t (*stop)(void);   // flushes the current dwell; idempotent
} link_survey_ops_t;

// Fills the BoardStatus fields link cannot know: current_channel,
// rx_frames_per_sec, dropped_rx_frames, gps_state. Called from the link task.
typedef void (*link_status_hook_t)(ns_board_v1_BoardStatus *status);

// Handles a command that belongs to another module (pairing, OTA). Sets
// resp->result (and payload or message); request_id is already filled.
// Called from the link task.
typedef void (*link_command_handler_t)(uint16_t conn, const ns_board_v1_ControlRequest *req,
                                       ns_board_v1_ControlResponse *resp);

// Handles an OTA characteristic write (offset + image bytes). Called from the
// NimBLE host task: copy or queue, do not block.
typedef void (*link_ota_write_t)(uint16_t conn, const uint8_t *data, uint16_t len);

// Registers with the GATT server and starts the link task. Call before
// gatt_server_init.
esp_err_t link_init(const link_survey_ops_t *ops, link_status_hook_t status_hook);

// Record sink. Thread-safe and non-blocking (a short lock around trimming,
// encoding and a copy); not for ISRs. board_time_ms in rec must be monotonic
// milliseconds (link_mono_ms()); the sink converts it to Unix time once SetTime
// has arrived, trims a copy to fit the MTU (ns_proto_fit_record), encodes it
// and queues it. When the ring is full the oldest records are dropped.
// Returns false if this record was dropped.
bool link_submit_record(const ns_board_v1_Record *rec);

// Raw frame sink for the Raw characteristic, same rules (board_time_us is
// monotonic microseconds). Raw frames are sent only while the phone is
// subscribed to Raw and Configure.raw_frames is on; rate limiting and the
// snaplen (link_raw_snaplen) are the caller's.
bool link_submit_raw(const ns_board_v1_RawFrame *frame);

// Monotonic board clock in milliseconds since boot.
uint64_t link_mono_ms(void);

// Board time for a monotonic time: Unix epoch ms after SetTime, else unchanged.
uint64_t link_board_time_ms(uint64_t mono_ms, bool *synced);

// Effective raw snaplen (protocol.md section 9) at the current MTU.
uint32_t link_raw_snaplen(void);

// Raw frames would be sent now: Configure.raw_frames is on, the board is
// surveying and a phone is subscribed to Raw. Capture code checks this so it
// does not copy frames nobody receives.
bool link_raw_wanted(void);

// Current configuration (protocol default until the first Configure).
void link_get_config(ns_board_v1_Configure *out);

ns_board_v1_BoardState link_board_state(void);

// For the OTA module: enter or leave BOARD_STATE_OTA (stops surveying).
void link_set_ota_active(bool active);

// For the OTA module: answer a compatible Hello with ota_image_confirmed when
// this returns true.
void link_set_hello_confirm_hook(bool (*confirm)(void));

// Registers a handler for EnterPairingMode, ForgetBonds, OtaBegin, OtaEnd or
// OtaAbort (command_tag is ns_board_v1_ControlRequest_*_tag). Unregistered
// commands answer RESULT_CODE_UNSUPPORTED.
void link_set_command_handler(pb_size_t command_tag, link_command_handler_t handler);

void link_set_ota_write_handler(link_ota_write_t handler);

// Sends an OtaAck on the OTA characteristic of conn. False if it could not be
// queued (not subscribed, or out of buffers).
bool link_send_ota_ack(uint16_t conn, const ns_board_v1_OtaAck *ack);

// Sends Status now (pairing mode, GPS state or bond changes).
void link_status_changed(void);

// Counters for drops that happen outside link (raw rate limit).
void link_count_raw_dropped(uint32_t n);
