// Firmware update over BLE (docs/protocol.md section 10): OtaBegin, image
// chunks on the OTA characteristic with OtaAck windowing, OtaEnd and OtaAbort.
// The image goes to the inactive OTA slot; the board checks its SHA-256 and
// format before switching to it. The Ed25519 signature is the app's job.
//
// A new image boots pending verification (bootloader rollback). It is
// confirmed by a compatible Hello while the self-test passes, or by the
// self-test after 60 s of uptime, whichever comes first; if it restarts before
// that, the bootloader returns to the old image.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Called with true when a transfer starts and false when it ends without a
// restart (abort, failure). From the link or OTA task.
typedef void (*ota_active_cb_t)(bool active);

// Reads the running image's state and registers the OTA commands, the OTA
// characteristic handler and the Hello confirmation with the link layer.
// Call before gatt_server_init.
esp_err_t ota_init(ota_active_cb_t on_active);

// Sets the self-test. healthy() must return true when the board is fully up
// (capture and link initialised, sniffer task running, BLE host synced); it
// gates the Hello confirmation. For an image pending verification it is also
// polled every few seconds from 60 s of uptime and confirms the image when it
// returns true. If it is still false (and the image unconfirmed) at 180 s, the
// board rolls back and restarts.
void ota_start_self_test(bool (*healthy)(void));

// True while the running image waits for confirmation.
bool ota_image_pending(void);
