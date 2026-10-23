// Control commands (docs/protocol.md section 6) and the survey state: Hello
// compatibility gate, SetTime, Configure, Start/Stop, Ping, and hand-off of
// pairing and OTA commands to the modules that register for them.

#include <string.h>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "gatt_server.h"
#include "link_internal.h"
#include "pb_decode.h"

static const char *TAG = "control";

// A board left without a bonded phone stops surveying after this long.
#define ORPHAN_STOP_MS (5 * 60 * 1000)
// SetTime values outside 2001 to 2100 are not a phone clock.
#define MIN_UNIX_MS 1000000000000ULL
#define MAX_UNIX_MS 4102444800000ULL

#define RC_OK ns_board_v1_ResultCode_RESULT_CODE_OK
#define RC_ERROR ns_board_v1_ResultCode_RESULT_CODE_ERROR
#define RC_INVALID_ARGUMENT ns_board_v1_ResultCode_RESULT_CODE_INVALID_ARGUMENT
#define RC_INVALID_STATE ns_board_v1_ResultCode_RESULT_CODE_INVALID_STATE
#define RC_UNSUPPORTED ns_board_v1_ResultCode_RESULT_CODE_UNSUPPORTED
#define RC_INCOMPATIBLE ns_board_v1_ResultCode_RESULT_CODE_INCOMPATIBLE

// Commands that belong to other modules, by ControlRequest oneof tag.
#define FIRST_EXT_TAG ns_board_v1_ControlRequest_enter_pairing_mode_tag
#define LAST_EXT_TAG ns_board_v1_ControlRequest_ota_abort_tag

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;  // s_config
static ns_board_v1_Configure s_config;
static bool s_config_applied;
static ns_board_v1_Role s_role;
static volatile ns_board_v1_BoardState s_state = ns_board_v1_BoardState_BOARD_STATE_IDLE;
static const link_survey_ops_t *s_ops;
static link_status_hook_t s_status_hook;
static link_command_handler_t s_handlers[LAST_EXT_TAG - FIRST_EXT_TAG + 1];
static bool (*s_hello_confirm)(void);
static int64_t s_orphan_since_ms = -1;

static void default_config(ns_board_v1_Configure *cfg)
{
    *cfg = (ns_board_v1_Configure)ns_board_v1_Configure_init_zero;
    cfg->wifi_ap = true;
    cfg->wifi_client = true;
    cfg->role = ns_board_v1_Role_ROLE_AUTO;
}

void link_control_init(const link_survey_ops_t *ops, link_status_hook_t hook)
{
    s_ops = ops;
    s_status_hook = hook;
    default_config(&s_config);
    s_role = s_config.role;
}

void link_get_config(ns_board_v1_Configure *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_config;
    taskEXIT_CRITICAL(&s_mux);
}

ns_board_v1_BoardState link_board_state(void)
{
    return s_state;
}

ns_board_v1_Role link_control_role(void)
{
    return s_role;
}

bool link_control_raw_enabled(void)
{
    return s_config.raw_frames && s_state == ns_board_v1_BoardState_BOARD_STATE_SURVEYING;
}

void link_set_hello_confirm_hook(bool (*confirm)(void))
{
    s_hello_confirm = confirm;
}

void link_set_command_handler(pb_size_t command_tag, link_command_handler_t handler)
{
    if (command_tag >= FIRST_EXT_TAG && command_tag <= LAST_EXT_TAG) {
        s_handlers[command_tag - FIRST_EXT_TAG] = handler;
    }
}

static void stop_survey(void)
{
    if (s_ops != NULL) {
        s_ops->stop();
    }
    s_state = ns_board_v1_BoardState_BOARD_STATE_IDLE;
}

void link_set_ota_active(bool active)
{
    if (active) {
        if (s_state == ns_board_v1_BoardState_BOARD_STATE_SURVEYING) {
            stop_survey();
        }
        s_state = ns_board_v1_BoardState_BOARD_STATE_OTA;
    } else if (s_state == ns_board_v1_BoardState_BOARD_STATE_OTA) {
        s_state = ns_board_v1_BoardState_BOARD_STATE_IDLE;
    }
    link_status_changed();
}

void link_control_tick(bool bonded_phone_connected)
{
    int64_t now = (int64_t)link_mono_ms();
    if (bonded_phone_connected || s_state != ns_board_v1_BoardState_BOARD_STATE_SURVEYING) {
        s_orphan_since_ms = -1;
    } else if (s_orphan_since_ms < 0) {
        s_orphan_since_ms = now;
    } else if (now - s_orphan_since_ms >= ORPHAN_STOP_MS) {
        ESP_LOGW(TAG, "no phone for 5 minutes, stopping the survey");
        stop_survey();
        s_orphan_since_ms = -1;
    }
}

void link_control_fill_status(ns_board_v1_BoardStatus *st)
{
    uint64_t mono = link_mono_ms();
    st->board_time_ms = link_board_time_ms(mono, &st->time_synced);
    st->state = s_state;
    st->role = s_role;
    st->uptime_ms = mono;
    st->free_heap_bytes = esp_get_free_heap_size();
    st->min_free_heap_bytes = esp_get_minimum_free_heap_size();
    st->gps_state = board_gps_present() ? ns_board_v1_GpsState_GPS_STATE_NO_FIX
                                        : ns_board_v1_GpsState_GPS_STATE_NOT_PRESENT;
    uint32_t mv;
    if (board_has_battery() && board_battery_mv(&mv) == ESP_OK) {
        st->has_battery_mv = true;
        st->battery_mv = mv;
    }
    st->pairing_mode = gatt_server_pairable();
    st->bond_count = (uint32_t)gatt_server_bond_count();
    if (s_status_hook != NULL) {
        s_status_hook(st);
    }
}

// ---------------------------------------------------------------------------
// Commands

static void fail(ns_board_v1_ControlResponse *resp, ns_board_v1_ResultCode code, const char *msg)
{
    resp->result = code;
    strlcpy(resp->message, msg, sizeof(resp->message));
}

static void on_hello(const ns_board_v1_Hello *hello, bool *hello_ok, ns_board_v1_ControlResponse *resp)
{
    ns_board_v1_HelloResult *r = &resp->payload.hello;
    resp->which_payload = ns_board_v1_ControlResponse_hello_tag;
    r->protocol_major = ns_board_v1_ProtocolVersion_PROTOCOL_VERSION_MAJOR;
    r->protocol_minor = ns_board_v1_ProtocolVersion_PROTOCOL_VERSION_MINOR;
    r->compatible = hello->protocol_major == ns_board_v1_ProtocolVersion_PROTOCOL_VERSION_MAJOR;
    strlcpy(r->firmware_version, esp_app_get_description()->version, sizeof(r->firmware_version));
    if (r->compatible && s_hello_confirm != NULL) {
        r->ota_image_confirmed = s_hello_confirm();
    }
    *hello_ok = r->compatible;
    resp->result = r->compatible ? RC_OK : RC_INCOMPATIBLE;
    ESP_LOGI(TAG, "Hello from app %s, protocol %u.%u: %s", hello->app_version, (unsigned)hello->protocol_major,
             (unsigned)hello->protocol_minor, r->compatible ? "compatible" : "incompatible");
}

static void on_set_time(const ns_board_v1_SetTime *t, ns_board_v1_ControlResponse *resp)
{
    if (t->unix_time_ms < MIN_UNIX_MS || t->unix_time_ms > MAX_UNIX_MS) {
        fail(resp, RC_INVALID_ARGUMENT, "time out of range");
        return;
    }
    link_clock_set_unix(t->unix_time_ms);
    resp->result = RC_OK;
}

static ns_board_v1_ResultCode apply(const ns_board_v1_Configure *cfg, ns_board_v1_ConfigureResult *result,
                                    const char **why)
{
    if (s_ops == NULL) {
        *why = "no capture backend";
        return RC_ERROR;
    }
    esp_err_t err = s_ops->configure(cfg, result);
    if (err == ESP_ERR_INVALID_ARG) {
        *why = "no usable channel";
        return RC_INVALID_ARGUMENT;
    }
    if (err != ESP_OK) {
        *why = esp_err_to_name(err);
        return RC_ERROR;
    }
    taskENTER_CRITICAL(&s_mux);
    s_config = *cfg;
    taskEXIT_CRITICAL(&s_mux);
    s_config_applied = true;
    s_role = result->role;
    return RC_OK;
}

static void on_configure(const ns_board_v1_Configure *cfg, ns_board_v1_ControlResponse *resp)
{
    if (s_state == ns_board_v1_BoardState_BOARD_STATE_OTA) {
        fail(resp, RC_INVALID_STATE, "firmware update in progress");
        return;
    }
    if (cfg->role < ns_board_v1_Role_ROLE_AUTO || cfg->role > ns_board_v1_Role_ROLE_BLE_SNIFFER) {
        fail(resp, RC_INVALID_ARGUMENT, "unknown role");
        return;
    }
    if (cfg->role == ns_board_v1_Role_ROLE_FIXED_CHANNEL && cfg->channels_count == 0) {
        fail(resp, RC_INVALID_ARGUMENT, "fixed channel needs a channel");
        return;
    }
    ns_board_v1_ConfigureResult *result = &resp->payload.configure;
    *result = (ns_board_v1_ConfigureResult)ns_board_v1_ConfigureResult_init_zero;
    const char *why = NULL;
    resp->result = apply(cfg, result, &why);
    if (resp->result != RC_OK) {
        fail(resp, resp->result, why);
        return;
    }
    result->raw_snaplen = link_raw_snaplen();
    resp->which_payload = ns_board_v1_ControlResponse_configure_tag;
    ESP_LOGI(TAG, "configured: role %d, %u channels", result->role, (unsigned)result->channels_count);
}

static void on_start(ns_board_v1_ControlResponse *resp)
{
    if (s_state == ns_board_v1_BoardState_BOARD_STATE_OTA) {
        fail(resp, RC_INVALID_STATE, "firmware update in progress");
        return;
    }
    if (s_state == ns_board_v1_BoardState_BOARD_STATE_SURVEYING) {
        resp->result = RC_OK;
        return;
    }
    if (!s_config_applied) {
        static ns_board_v1_ConfigureResult result;
        ns_board_v1_Configure cfg;
        default_config(&cfg);
        const char *why = NULL;
        if (apply(&cfg, &result, &why) != RC_OK) {
            fail(resp, RC_ERROR, why);
            return;
        }
    }
    esp_err_t err = s_ops->start();
    if (err != ESP_OK) {
        fail(resp, RC_ERROR, esp_err_to_name(err));
        return;
    }
    s_state = ns_board_v1_BoardState_BOARD_STATE_SURVEYING;
    resp->result = RC_OK;
    ESP_LOGI(TAG, "survey started");
}

static void on_stop(ns_board_v1_ControlResponse *resp)
{
    if (s_state == ns_board_v1_BoardState_BOARD_STATE_SURVEYING) {
        stop_survey();
        ESP_LOGI(TAG, "survey stopped");
    }
    resp->result = RC_OK;
}

static void on_ping(const ns_board_v1_Ping *ping, ns_board_v1_ControlResponse *resp)
{
    ns_board_v1_PingResult *r = &resp->payload.ping;
    resp->which_payload = ns_board_v1_ControlResponse_ping_tag;
    r->nonce = ping->nonce;
    r->board_time_ms = link_board_time_ms(link_mono_ms(), &r->time_synced);
    resp->result = RC_OK;
}

// Commands that never change between major versions, allowed before Hello.
static bool frozen_command(pb_size_t tag)
{
    switch (tag) {
    case ns_board_v1_ControlRequest_hello_tag:
    case ns_board_v1_ControlRequest_ping_tag:
    case ns_board_v1_ControlRequest_forget_bonds_tag:
    case ns_board_v1_ControlRequest_ota_begin_tag:
    case ns_board_v1_ControlRequest_ota_end_tag:
    case ns_board_v1_ControlRequest_ota_abort_tag:
        return true;
    default:
        return false;
    }
}

// request_id of a write that does not decode as a whole: field 1 if readable.
static uint32_t salvage_request_id(const uint8_t *data, size_t len)
{
    pb_istream_t is = pb_istream_from_buffer(data, len);
    pb_wire_type_t wire;
    uint32_t tag;
    bool eof;
    while (pb_decode_tag(&is, &wire, &tag, &eof)) {
        if (tag == ns_board_v1_ControlRequest_request_id_tag && wire == PB_WT_VARINT) {
            uint32_t id;
            return pb_decode_varint32(&is, &id) ? id : 0;
        }
        if (!pb_skip_field(&is, wire)) {
            break;
        }
    }
    return 0;
}

void link_control_handle(uint16_t conn, bool *hello_ok, const uint8_t *data, size_t len,
                         ns_board_v1_ControlResponse *resp)
{
    static ns_board_v1_ControlRequest req;
    *resp = (ns_board_v1_ControlResponse)ns_board_v1_ControlResponse_init_zero;
    req = (ns_board_v1_ControlRequest)ns_board_v1_ControlRequest_init_zero;
    pb_istream_t is = pb_istream_from_buffer(data, len);
    if (!pb_decode(&is, ns_board_v1_ControlRequest_fields, &req)) {
        resp->request_id = salvage_request_id(data, len);
        fail(resp, RC_INVALID_ARGUMENT, PB_GET_ERROR(&is));
        ESP_LOGW(TAG, "request %u does not decode: %s", (unsigned)resp->request_id, PB_GET_ERROR(&is));
        return;
    }
    resp->request_id = req.request_id;
    pb_size_t cmd = req.which_command;
    if (cmd == 0) {
        fail(resp, RC_UNSUPPORTED, "unknown command");
        return;
    }
    if (!*hello_ok && !frozen_command(cmd)) {
        fail(resp, RC_INVALID_STATE, "send Hello first");
        return;
    }
    switch (cmd) {
    case ns_board_v1_ControlRequest_hello_tag:
        on_hello(&req.command.hello, hello_ok, resp);
        break;
    case ns_board_v1_ControlRequest_set_time_tag:
        on_set_time(&req.command.set_time, resp);
        break;
    case ns_board_v1_ControlRequest_configure_tag:
        on_configure(&req.command.configure, resp);
        break;
    case ns_board_v1_ControlRequest_start_tag:
        on_start(resp);
        break;
    case ns_board_v1_ControlRequest_stop_tag:
        on_stop(resp);
        break;
    case ns_board_v1_ControlRequest_ping_tag:
        on_ping(&req.command.ping, resp);
        break;
    default:
        if (cmd >= FIRST_EXT_TAG && cmd <= LAST_EXT_TAG && s_handlers[cmd - FIRST_EXT_TAG] != NULL) {
            s_handlers[cmd - FIRST_EXT_TAG](conn, &req, resp);
        } else {
            fail(resp, RC_UNSUPPORTED, "not implemented");
        }
        break;
    }
}
