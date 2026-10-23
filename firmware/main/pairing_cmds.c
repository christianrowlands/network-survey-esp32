#include "pairing_cmds.h"

#include <string.h>

#include "esp_log.h"
#include "link.h"

static const char *TAG = "pairing";

// Time for the ForgetBonds response to reach the phone before the board
// deletes the bonds and disconnects.
#define FORGET_DELAY_MS 500
#define WIPE_CONFIRM_MS 2000

static void on_enter_pairing_mode(uint16_t conn, const ns_board_v1_ControlRequest *req,
                                  ns_board_v1_ControlResponse *resp)
{
    (void)req;
    if (gatt_server_enter_pairing_mode(conn)) {
        resp->result = ns_board_v1_ResultCode_RESULT_CODE_OK;
    } else {
        resp->result = ns_board_v1_ResultCode_RESULT_CODE_INVALID_STATE;
        strlcpy(resp->message, "connection is not bonded", sizeof(resp->message));
    }
}

static void on_forget_bonds(uint16_t conn, const ns_board_v1_ControlRequest *req,
                            ns_board_v1_ControlResponse *resp)
{
    (void)conn;
    (void)req;
    ESP_LOGW(TAG, "ForgetBonds: deleting every bond");
    gatt_server_forget_bonds(FORGET_DELAY_MS);
    resp->result = ns_board_v1_ResultCode_RESULT_CODE_OK;
}

void pairing_cmds_register(void)
{
    link_set_command_handler(ns_board_v1_ControlRequest_enter_pairing_mode_tag, on_enter_pairing_mode);
    link_set_command_handler(ns_board_v1_ControlRequest_forget_bonds_tag, on_forget_bonds);
}

void pairing_cmds_on_button(ui_button_event_t event)
{
    switch (event) {
    case UI_BUTTON_HOLD_3S:
        gatt_server_open_pairing_window();
        break;
    case UI_BUTTON_HOLD_10S_AT_BOOT:
        gatt_server_forget_bonds(0);
        break;
    case UI_BUTTON_SHORT_PRESS:
        break;
    }
}

void pairing_cmds_on_gatt(gatt_event_t event)
{
    switch (event) {
    case GATT_EVENT_PAIRING_CHANGED:
        ui_io_led_set(UI_LED_PAIRING, gatt_server_pairable());
        link_status_changed();
        break;
    case GATT_EVENT_BONDS_WIPED:
        ui_io_led_pulse(UI_LED_CONFIRM, WIPE_CONFIRM_MS);
        break;
    default:
        break;
    }
}
