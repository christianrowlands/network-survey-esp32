// Bond gate and pairing window (docs/protocol.md section 3) on NimBLE. The
// decisions come from pairing_policy.c; this file feeds it events and applies
// the results. Everything here runs in the NimBLE host task; other tasks only
// post requests (gatt_server_open_pairing_window and friends).
//
// How a stranger is kept out while the board is not pairable:
// - sm_sec_lvl 1 makes NimBLE answer every pairing request with "Command Not
//   Supported" (it is 0, the default, while pairable).
// - The bond store callback refuses to make room for a new bond, and never
//   persists one, outside the window.
// - A repeated pairing (a phone that lost its keys) is ignored.
// - Encrypted characteristics and their CCCDs need a link encrypted with keys
//   that are still in the store (gatt_link_bonded).

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "gatt_internal.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "pairing_policy.h"
#include "sdkconfig.h"

static const char *TAG = "pairing";

#define TICK_MS 1000

#define REQ_OPEN_WINDOW 0x1
#define REQ_ENTER_PAIRING 0x2
#define REQ_WIPE 0x4

static pp_state_t s_policy;
static bool s_inited;
static bool s_started;
static volatile bool s_pairable = true;  // cached for other tasks
static volatile int s_bonds;
static volatile uint32_t s_bonded_mask;  // bit per connection handle, for other tasks
static uint32_t s_requests;
static volatile uint16_t s_enter_conn;
static struct ble_npl_event s_request_ev;
static struct ble_npl_callout s_tick;
static struct ble_npl_callout s_wipe_timer;
static ble_store_write_fn *s_store_write;
static bool s_new_bond;  // peer keys of s_new_bond_peer were just stored
static ble_addr_t s_new_bond_peer;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static int count_bonds(void)
{
    int n = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n);
    return n;
}

static void update_bonded_mask(void)
{
    uint32_t mask = 0;
    for (int i = 0; i < s_policy.conn_count; i++) {
        if (s_policy.conns[i].bonded && s_policy.conns[i].handle < 32) {
            mask |= 1u << s_policy.conns[i].handle;
        }
    }
    s_bonded_mask = mask;
}

// Carries out a policy result: drops connections, updates the pairing gate,
// the cached state and advertising, and tells main when pairable or the bond
// count changed.
static void apply(const pp_out_t *out)
{
    for (int i = 0; i < out->drop_count; i++) {
        ESP_LOGI(TAG, "dropping connection %d", out->drop[i]);
        ble_gap_terminate(out->drop[i], BLE_ERR_REM_USER_CONN_TERM);
    }
    bool pairable = pp_pairable(&s_policy);
    ble_hs_cfg.sm_sec_lvl = pairable ? 0 : 1;
    update_bonded_mask();
    if (out->changed || pairable != s_pairable || s_policy.bonds != s_bonds) {
        s_pairable = pairable;
        s_bonds = s_policy.bonds;
        ESP_LOGI(TAG, "%s, %d bond(s)%s", pairable ? "pairable" : "not pairable", s_policy.bonds,
                 s_policy.window ? ", window open" : "");
        gap_emit(GATT_EVENT_PAIRING_CHANGED, BLE_HS_CONN_HANDLE_NONE);
    }
    gap_advertise();
}

// ble_gap_unpair (it also updates the resolving list) refuses while advertising
// or scanning. Advertising returns with the next policy result; the BLE sniffer
// restarts its scan by itself within about a second.
static void quiesce_radio(void)
{
    gap_adv_stop();
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
}

// Deletes every bond and drops every phone (the store is cleared regardless).
static void wipe_bonds(void)
{
    quiesce_radio();
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, CONFIG_BT_NIMBLE_MAX_BONDS) == 0) {
        for (int i = 0; i < n; i++) {
            int rc = ble_gap_unpair(&peers[i]);
            if (rc != 0) {
                ESP_LOGW(TAG, "unpair: rc=%d", rc);
            }
        }
    }
    int rc = ble_store_clear();
    ESP_LOGW(TAG, "all bonds deleted (rc=%d)", rc);
    pp_out_t out;
    pp_bonds_wiped(&s_policy, &out);
    apply(&out);
    gap_emit(GATT_EVENT_BONDS_WIPED, BLE_HS_CONN_HANDLE_NONE);
}

// Makes room for a new bond during the window: deletes the oldest bond and
// disconnects its phone (protocol.md section 3).
static int replace_oldest_bond(void)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int n = 0;
    int rc = ble_store_util_bonded_peers(peers, &n, CONFIG_BT_NIMBLE_MAX_BONDS);
    if (rc != 0 || n == 0) {
        return rc != 0 ? rc : BLE_HS_ENOENT;
    }
    quiesce_radio();
    // That phone loses access now; the policy is applied (and advertising
    // restarted) when the new pairing reports its encryption change.
    struct ble_gap_conn_desc desc;
    bool connected = ble_gap_conn_find_by_addr(&peers[0], &desc) == 0;
    if (connected) {
        pp_out_t out;
        pp_link_unbonded(&s_policy, desc.conn_handle, s_policy.bonds, &out);
        update_bonded_mask();
    }
    rc = ble_gap_unpair(&peers[0]);  // also disconnects that phone
    if (rc != 0) {
        // Still busy: delete and disconnect by hand.
        ESP_LOGW(TAG, "unpair oldest: rc=%d, deleting directly", rc);
        if (connected) {
            ble_gap_terminate(desc.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        rc = ble_store_util_delete_peer(&peers[0]);
    }
    ESP_LOGI(TAG, "replaced the oldest bond (rc=%d)", rc);
    return rc;
}

static int store_status(struct ble_store_status_event *event, void *arg)
{
    (void)arg;
    pp_pair_t decision = pp_pair_request(&s_policy);
    switch (event->event_code) {
    case BLE_STORE_EVENT_FULL:
        // A pairing request while the store is full.
        if (decision == PP_PAIR_DENY) {
            ESP_LOGW(TAG, "pairing request refused: not pairable");
            return BLE_HS_ESTORE_CAP;
        }
        return 0;
    case BLE_STORE_EVENT_OVERFLOW:
        switch (event->overflow.obj_type) {
        case BLE_STORE_OBJ_TYPE_OUR_SEC:
        case BLE_STORE_OBJ_TYPE_PEER_SEC:
        case BLE_STORE_OBJ_TYPE_PEER_ADDR:
            if (decision == PP_PAIR_DENY) {
                ESP_LOGW(TAG, "bond store full outside the pairing window, not storing");
                return BLE_HS_ESTORE_CAP;
            }
            return replace_oldest_bond();
        default:
            // CCCDs and the like: never evict a bond for them. The app
            // subscribes again on every connection.
            return BLE_HS_ESTORE_CAP;
        }
    default:
        return BLE_HS_EUNKNOWN;
    }
}

// Notes a new bond: NimBLE writes the peer keys only when a pairing with
// bonding completes, just before it reports the encryption change.
static int store_write(int obj_type, const union ble_store_value *val)
{
    int rc = s_store_write(obj_type, val);
    if (rc == 0 && obj_type == BLE_STORE_OBJ_TYPE_PEER_SEC) {
        s_new_bond = true;
        s_new_bond_peer = val->sec.peer_addr;
    }
    return rc;
}

bool gatt_link_bonded(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0 || !desc.sec_state.encrypted || !desc.sec_state.bonded ||
        desc.sec_state.key_size < 16) {
        return false;
    }
    // The bond may have been deleted since the link was encrypted.
    struct ble_store_key_sec key = {.peer_addr = desc.peer_id_addr};
    struct ble_store_value_sec value;
    return ble_store_read_peer_sec(&key, &value) == 0;
}

void pairing_on_connect(uint16_t conn)
{
    pp_out_t out;
    pp_connected(&s_policy, conn, now_ms(), &out);
    apply(&out);
}

void pairing_on_disconnect(uint16_t conn)
{
    pp_out_t out;
    pp_disconnected(&s_policy, conn, &out);
    apply(&out);
}

void pairing_on_enc_change(uint16_t conn, int status)
{
    pp_out_t out;
    if (status != 0) {
        pp_link_unbonded(&s_policy, conn, count_bonds(), &out);
        apply(&out);
        return;
    }
    bool bonded = gatt_link_bonded(conn);
    // The keys stored just before belong to this connection (with two phones
    // in a window, another one may re-encrypt in between).
    struct ble_gap_conn_desc desc;
    bool found = ble_gap_conn_find(conn, &desc) == 0;
    bool new_bond = s_new_bond && found && ble_addr_cmp(&s_new_bond_peer, &desc.peer_id_addr) == 0;
    if (s_new_bond && !new_bond) {
        ESP_LOGW(TAG, "connection %d encrypted while keys of another peer were just stored", conn);
    }
    if (new_bond) {
        s_new_bond = false;
        struct ble_store_key_sec key = {.peer_addr = desc.peer_id_addr};
        struct ble_store_value_sec value = {0};
        ble_store_read_peer_sec(&key, &value);
        ESP_LOGI(TAG, "new bond on connection %d (identity key %s)", conn,
                 value.irk_present ? "stored" : "not sent");
    }
    pp_encrypted(&s_policy, conn, bonded, new_bond, count_bonds(), &out);
    apply(&out);
}

int pairing_on_repeat_pairing(uint16_t conn)
{
    if (pp_pair_request(&s_policy) == PP_PAIR_DENY) {
        ESP_LOGW(TAG, "repeat pairing outside the pairing window, ignored");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    // The phone lost its keys: drop its old bond (keep the link) and pair again.
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) == 0) {
        ble_store_util_delete_peer(&desc.peer_id_addr);
    }
    pp_out_t out;
    pp_link_unbonded(&s_policy, conn, count_bonds(), &out);
    apply(&out);
    return BLE_GAP_REPEAT_PAIRING_RETRY;
}

static void on_tick(struct ble_npl_event *ev)
{
    (void)ev;
    pp_out_t out;
    pp_tick(&s_policy, now_ms(), &out);
    if (out.changed || out.drop_count > 0) {
        apply(&out);
    }
    ble_npl_callout_reset(&s_tick, ble_npl_time_ms_to_ticks32(TICK_MS));
}

static void on_request(struct ble_npl_event *ev)
{
    (void)ev;
    if (!s_started) {
        return;  // kept until the host has synced again (pairing_on_sync posts)
    }
    uint32_t req = __atomic_exchange_n(&s_requests, 0, __ATOMIC_ACQ_REL);
    pp_out_t out;
    if (req & REQ_WIPE) {
        wipe_bonds();
    }
    if (req & REQ_OPEN_WINDOW) {
        ESP_LOGI(TAG, "pairing window opened (button)");
        pp_open_window(&s_policy, now_ms(), &out);
        apply(&out);
    }
    if (req & REQ_ENTER_PAIRING) {
        if (pp_enter_pairing_mode(&s_policy, s_enter_conn, now_ms(), &out)) {
            ESP_LOGI(TAG, "pairing window opened (EnterPairingMode)");
            apply(&out);
        }
    }
}

static void on_wipe_timer(struct ble_npl_event *ev)
{
    (void)ev;
    if (s_started) {
        wipe_bonds();
    }
}

static void post(uint32_t req)
{
    if (!s_inited) {
        return;
    }
    __atomic_fetch_or(&s_requests, req, __ATOMIC_ACQ_REL);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_request_ev);
}

void pairing_init(void)
{
    pp_init(&s_policy, CONFIG_BT_NIMBLE_MAX_BONDS, CONFIG_BT_NIMBLE_MAX_CONNECTIONS, 0);
    ble_npl_event_init(&s_request_ev, on_request, NULL);
    ble_npl_callout_init(&s_tick, nimble_port_get_dflt_eventq(), on_tick, NULL);
    ble_npl_callout_init(&s_wipe_timer, nimble_port_get_dflt_eventq(), on_wipe_timer, NULL);
    ble_hs_cfg.store_status_cb = store_status;
    // Call after ble_store_config_init, which installs the NVS store.
    s_store_write = ble_hs_cfg.store_write_cb;
    ble_hs_cfg.store_write_cb = store_write;
    s_inited = true;
}

void pairing_on_sync(void)
{
    s_started = true;
    // Fresh state: connections from before a host reset are gone.
    pp_init(&s_policy, CONFIG_BT_NIMBLE_MAX_BONDS, CONFIG_BT_NIMBLE_MAX_CONNECTIONS, count_bonds());
    pp_out_t out = {.changed = true};  // first report: LED and Status start from here
    apply(&out);
    ble_npl_callout_reset(&s_tick, ble_npl_time_ms_to_ticks32(TICK_MS));
    if (__atomic_load_n(&s_requests, __ATOMIC_ACQUIRE) != 0) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_request_ev);
    }
}

void pairing_on_reset(void)
{
    s_started = false;
}

pp_adv_t pairing_advertising(void)
{
    return pp_advertising(&s_policy);
}

void gatt_server_open_pairing_window(void)
{
    post(REQ_OPEN_WINDOW);
}

// Other tasks read the host task's per-connection flag, never the bond store
// (its arrays have no lock).
bool gatt_server_link_bonded(uint16_t conn)
{
    return conn < 32 && (s_bonded_mask & (1u << conn)) != 0;
}

bool gatt_server_enter_pairing_mode(uint16_t conn)
{
    if (!gatt_server_link_bonded(conn)) {
        return false;
    }
    s_enter_conn = conn;
    post(REQ_ENTER_PAIRING);
    return true;
}

void gatt_server_forget_bonds(uint32_t delay_ms)
{
    if (!s_inited) {
        return;
    }
    if (delay_ms == 0) {
        post(REQ_WIPE);
    } else {
        ble_npl_callout_reset(&s_wipe_timer, ble_npl_time_ms_to_ticks32(delay_ms));
    }
}

bool gatt_server_pairable(void)
{
    return s_pairable;
}

int gatt_server_bond_count(void)
{
    return s_bonds;
}
