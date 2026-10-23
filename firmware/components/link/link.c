// The link task: sends Control indications, Status and Records/Raw
// notifications. It owns all per-connection state; the GATT callbacks in
// link_gatt.c only post events or fill Control request slots, so no lock is
// held while calling into NimBLE.

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gatt_server.h"
#include "host/ble_hs.h"
#include "link_frame.h"
#include "link_internal.h"
#include "pb_encode.h"
#include "sdkconfig.h"

static const char *TAG = "link";

#define TICK_MS 20
// Records and Raw wait for at least the protocol's minimum MTU.
#define MIN_DATA_MTU 185
// A Control indication without a confirmation event for this long is
// considered lost (the ATT timeout is 30 s).
#define INDICATE_TIMEOUT_MS 35000
#define STATUS_PERIOD_MS 1000
#define MAX_PEERS CONFIG_BT_NIMBLE_MAX_CONNECTIONS
// Frames sent per tick and characteristic. Records and Raw stop while fewer
// than half the NimBLE buffers are free: that keeps buffers for Status and
// Control (protocol.md: they take priority) and bounds how long they queue
// behind records (measured on a C5 at saturation: about 0.25 s).
#define FRAMES_PER_TICK 8
#define TX_RESERVE ((CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT + CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT) / 2)

typedef struct {
    bool in_use;
    bool hello_ok;
    bool indicating;  // a Control indication awaits confirmation
    int64_t indicated_ms;
    uint16_t conn;
    bool subscribed[GATT_CHR_COUNT];
    link_seq_t seq[GATT_CHR_COUNT];
    uint16_t ctl_len;  // framed response waiting to be indicated
    uint8_t ctl_frame[LINK_FRAME_MAX];
} peer_t;

// A frame built from a ring, kept until the stack accepts it.
typedef struct {
    uint8_t buf[LINK_FRAME_MAX];
    size_t len;
    int count;       // records or raw frames in it
    bool stamped;    // has a sequence number of the current subscription
    uint16_t owner;  // connection that stamped it
} pending_t;

static peer_t s_peers[MAX_PEERS];
static pending_t s_rec_pending;
static pending_t s_raw_pending;
static volatile bool s_status_due;
static int64_t s_last_status_ms;
static uint32_t s_records_per_sec;

void link_status_changed(void)
{
    s_status_due = true;
}

// ---------------------------------------------------------------------------
// Link task

static peer_t *find_peer(uint16_t conn)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].in_use && s_peers[i].conn == conn) {
            return &s_peers[i];
        }
    }
    return NULL;
}

// The connection that receives Records and Raw: the first one past a
// compatible Hello that is subscribed.
static peer_t *data_peer(gatt_chr_t chr)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        peer_t *p = &s_peers[i];
        if (p->in_use && p->hello_ok && p->subscribed[chr]) {
            return p;
        }
    }
    return NULL;
}

static bool soft_error(int rc)
{
    return rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY || rc == BLE_HS_ESTALLED;
}

// A kept frame gets a new number from the next subscription.
static void unstamp(uint16_t conn, gatt_chr_t chr)
{
    if (s_rec_pending.owner == conn && (chr == GATT_CHR_RECORDS || chr == GATT_CHR_COUNT)) {
        s_rec_pending.stamped = false;
    }
    if (s_raw_pending.owner == conn && (chr == GATT_CHR_RAW || chr == GATT_CHR_COUNT)) {
        s_raw_pending.stamped = false;
    }
}

static void handle_event(const link_event_t *ev)
{
    peer_t *p = find_peer(ev->conn);
    switch (ev->type) {
    case LINK_EV_CONNECTED:
        for (int i = 0; i < MAX_PEERS && p == NULL; i++) {
            if (!s_peers[i].in_use) {
                p = &s_peers[i];
            }
        }
        if (p != NULL) {  // a reused entry for this handle starts afresh too
            memset(p, 0, sizeof(*p));
            p->in_use = true;
            p->conn = ev->conn;
        }
        break;
    case LINK_EV_DISCONNECTED:
        if (p != NULL) {
            p->in_use = false;
        }
        unstamp(ev->conn, GATT_CHR_COUNT);
        break;
    case LINK_EV_SUBSCRIBED:
        if (p != NULL && ev->chr < GATT_CHR_COUNT) {
            p->subscribed[ev->chr] = ev->enabled;
            if (ev->enabled) {
                link_seq_reset(&p->seq[ev->chr]);
                unstamp(ev->conn, (gatt_chr_t)ev->chr);
                if (ev->chr == GATT_CHR_STATUS) {
                    s_status_due = true;
                }
            }
        }
        break;
    case LINK_EV_INDICATE_DONE:
        if (p != NULL && ev->chr == GATT_CHR_CONTROL) {
            p->indicating = false;
            if (ev->status != BLE_HS_EDONE) {
                ESP_LOGW(TAG, "Control indication failed: %d", ev->status);
            }
        }
        break;
    case LINK_EV_OTA_ACK:
        if (p != NULL && p->subscribed[GATT_CHR_OTA]) {
            uint8_t frame[2 + 1 + sizeof(ev->data)];
            link_frame_t f;
            link_frame_init(&f, frame, sizeof(frame), link_seq_take(&p->seq[GATT_CHR_OTA]));
            link_frame_add(&f, ev->data, ev->len);
            int rc = gatt_server_notify(p->conn, GATT_CHR_OTA, frame, (uint16_t)f.len);
            if (rc != 0) {
                ESP_LOGW(TAG, "OTA ack not sent: %d", rc);
            }
        }
        break;
    default:
        break;
    }
}

// Frames a ControlResponse; a response too large for the MTU becomes ERROR.
static void frame_response(peer_t *p, ns_board_v1_ControlResponse *resp)
{
    size_t payload = link_frame_payload(gatt_server_mtu(p->conn));
    uint8_t msg[ns_board_v1_ControlResponse_size];
    pb_ostream_t os = pb_ostream_from_buffer(msg, sizeof(msg));
    bool ok = pb_encode(&os, ns_board_v1_ControlResponse_fields, resp);
    if (!ok || os.bytes_written > link_frame_max_message(payload)) {
        ns_board_v1_ControlResponse err = ns_board_v1_ControlResponse_init_zero;
        err.request_id = resp->request_id;
        err.result = ns_board_v1_ResultCode_RESULT_CODE_ERROR;
        strlcpy(err.message, "response too large", sizeof(err.message));
        os = pb_ostream_from_buffer(msg, sizeof(msg));
        pb_encode(&os, ns_board_v1_ControlResponse_fields, &err);
    }
    link_frame_t f;
    link_frame_init(&f, p->ctl_frame, payload, p->seq[GATT_CHR_CONTROL].next);
    if (!link_frame_add(&f, msg, os.bytes_written)) {
        ESP_LOGW(TAG, "request %u: MTU %u too small for the response", (unsigned)resp->request_id,
                 (unsigned)gatt_server_mtu(p->conn));
        p->ctl_len = 0;
        return;
    }
    link_seq_take(&p->seq[GATT_CHR_CONTROL]);
    p->ctl_len = (uint16_t)f.len;
}

static void service_control(void)
{
    static uint8_t req[LINK_CTL_MAX];
    for (int i = 0; i < MAX_PEERS; i++) {
        peer_t *p = &s_peers[i];
        if (p->in_use && p->indicating && (int64_t)link_mono_ms() - p->indicated_ms > INDICATE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Control indication never confirmed");
            p->indicating = false;
        }
        if (!p->in_use || p->indicating) {
            continue;
        }
        if (p->ctl_len == 0) {
            size_t len;
            if (!link_gatt_take_request(p->conn, req, &len)) {
                continue;
            }
            static ns_board_v1_ControlResponse resp;
            link_control_handle(p->conn, &p->hello_ok, req, len, &resp);
            if (!p->subscribed[GATT_CHR_CONTROL]) {
                ESP_LOGW(TAG, "request %u: Control indications are off, no response", (unsigned)resp.request_id);
                continue;
            }
            frame_response(p, &resp);
            if (p->ctl_len == 0) {
                continue;
            }
        }
        int rc = gatt_server_indicate(p->conn, GATT_CHR_CONTROL, p->ctl_frame, p->ctl_len);
        if (rc == 0) {
            p->indicating = true;
            p->indicated_ms = (int64_t)link_mono_ms();
            p->ctl_len = 0;
        } else if (!soft_error(rc)) {
            ESP_LOGW(TAG, "Control response dropped: %d", rc);
            p->ctl_len = 0;
        }
    }
}

static bool wants_status(const peer_t *p)
{
    return p->in_use && p->hello_ok && p->subscribed[GATT_CHR_STATUS];
}

static void send_status(void)
{
    bool any = false;
    for (int i = 0; i < MAX_PEERS; i++) {
        any |= wants_status(&s_peers[i]);
    }
    s_status_due = false;
    if (!any) {
        return;  // also keeps NimBLE calls away until a phone is connected
    }
    static ns_board_v1_BoardStatus st;
    st = (ns_board_v1_BoardStatus)ns_board_v1_BoardStatus_init_zero;
    link_control_fill_status(&st);
    link_sink_fill_status(&st);
    st.records_per_sec = s_records_per_sec;

    for (int i = 0; i < MAX_PEERS; i++) {
        peer_t *p = &s_peers[i];
        if (!wants_status(p)) {
            continue;
        }
        st.mtu = gatt_server_mtu(p->conn);
        uint8_t msg[ns_board_v1_BoardStatus_size];
        pb_ostream_t os = pb_ostream_from_buffer(msg, sizeof(msg));
        uint8_t frame[LINK_FRAME_MAX];
        link_frame_t f;
        link_frame_init(&f, frame, link_frame_payload(st.mtu), p->seq[GATT_CHR_STATUS].next);
        if (!pb_encode(&os, ns_board_v1_BoardStatus_fields, &st) || !link_frame_add(&f, msg, os.bytes_written)) {
            continue;  // MTU not exchanged yet
        }
        int rc = gatt_server_notify(p->conn, GATT_CHR_STATUS, frame, (uint16_t)f.len);
        if (soft_error(rc)) {
            s_status_due = true;  // not sent: try again, with the same number
        } else {
            link_seq_take(&p->seq[GATT_CHR_STATUS]);
        }
    }
}

// Sends frames from one ring. Returns false when it stopped early (stack
// congestion), so lower priority traffic waits too.
static bool drain(gatt_chr_t chr, pending_t *pend)
{
    peer_t *p = data_peer(chr);
    if (p == NULL) {
        return true;
    }
    bool raw = chr == GATT_CHR_RAW;
    uint16_t mtu = gatt_server_mtu(p->conn);
    if (mtu < MIN_DATA_MTU) {
        // MTU not exchanged yet, or the connection just went away (0): wait
        // rather than discard records sized for a real MTU.
        return true;
    }
    if (!raw) {
        link_sink_set_fit_mtu(mtu);
    }
    if (pend->stamped && pend->owner != p->conn) {
        pend->stamped = false;  // the data connection changed
    }
    size_t payload = link_frame_payload(mtu);
    for (int i = 0; i < FRAMES_PER_TICK; i++) {
        if (pend->len == 0) {
            pend->len = raw ? link_sink_build_raw(pend->buf, payload, &pend->count)
                            : link_sink_build_records(pend->buf, payload, &pend->count);
            pend->stamped = false;
            if (pend->len == 0) {
                return true;
            }
        }
        if (pend->len > payload) {  // built for a larger MTU before a reconnect
            link_sink_count_lost(raw, (uint32_t)pend->count);
            pend->len = 0;
            continue;
        }
        if (gatt_server_tx_buffers_free() < TX_RESERVE) {
            return false;
        }
        if (!pend->stamped) {
            link_frame_set_seq(pend->buf, link_seq_take(&p->seq[chr]));
            pend->stamped = true;
            pend->owner = p->conn;
        }
        int rc = gatt_server_notify(p->conn, chr, pend->buf, (uint16_t)pend->len);
        if (soft_error(rc)) {
            return false;  // same frame and number next time
        }
        if (rc == BLE_HS_ENOTCONN) {
            pend->stamped = false;  // keep it for the next subscription
            return false;
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "chr %d frame dropped: %d", chr, rc);
            link_sink_count_lost(raw, (uint32_t)pend->count);
        }
        pend->len = 0;
    }
    return true;
}

static bool any_bonded_peer(void)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].in_use && gatt_server_link_bonded(s_peers[i].conn)) {
            return true;
        }
    }
    return false;
}

// Status goes out once a second and as soon as state or role change
// (protocol.md section 7). Pairing and GPS modules call link_status_changed.
static void service_status(void)
{
    static ns_board_v1_BoardState last_state;
    static ns_board_v1_Role last_role;
    int64_t now = (int64_t)link_mono_ms();
    if (now - s_last_status_ms >= STATUS_PERIOD_MS) {
        s_last_status_ms = now;
        s_records_per_sec = link_sink_take_produced();
        link_control_tick(any_bonded_peer());
        s_status_due = true;
    }
    ns_board_v1_BoardState state = link_board_state();
    ns_board_v1_Role role = link_control_role();
    if (state != last_state || role != last_role) {
        last_state = state;
        last_role = role;
        s_status_due = true;
    }
    if (s_status_due) {
        send_status();
    }
}

static void link_task(void *arg)
{
    (void)arg;
    for (;;) {
        link_event_t ev;
        if (link_gatt_next_event(&ev, pdMS_TO_TICKS(TICK_MS))) {
            do {
                handle_event(&ev);
            } while (link_gatt_next_event(&ev, 0));
        }
        service_control();
        service_status();
        link_sink_set_raw_listener(data_peer(GATT_CHR_RAW) != NULL);
        // Records before Raw: Raw only gets the link when Records is idle.
        if (drain(GATT_CHR_RECORDS, &s_rec_pending) && s_rec_pending.len == 0 && link_sink_records_empty()) {
            if (link_control_raw_enabled()) {
                drain(GATT_CHR_RAW, &s_raw_pending);
            } else {
                link_sink_clear_raw();
                s_raw_pending.len = 0;
            }
        }
    }
}

esp_err_t link_init(const link_survey_ops_t *ops, link_status_hook_t status_hook)
{
    link_control_init(ops, status_hook);
    esp_err_t err = link_sink_init();
    if (err != ESP_OK) {
        return err;
    }
    err = link_gatt_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(link_task, "link", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
