// BLE host setup, advertising and connection handling.

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "conn_track.h"
#include "esp_log.h"
#include "gatt_internal.h"
#include "gatt_server.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "ns_board.pb.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "gatt";

// Not declared in a public NimBLE header.
void ble_store_config_init(void);

#define COMPANY_ID 0xFFFF  // Bluetooth SIG test value until a registered ID exists
#define ADV_FLAG_GPS 0x04
#define ADV_FLAG_5GHZ 0x08

// 152.5 to 211.25 ms, intervals Apple recommends for accessories.
#define ADV_ITVL_MIN 244
#define ADV_ITVL_MAX 338

#define DLE_TX_OCTETS 251
#define DLE_TX_TIME_US 2120

uint8_t gatt_bt_address[6];
char gatt_short_id[5];

static char s_name[8];  // "NS-XXXX"
static uint8_t s_own_addr_type;
static volatile int s_conn_count;
static ct_state_t s_conns;  // connections reported to link and pairing (conn_track.h)
_Static_assert(CONFIG_BT_NIMBLE_MAX_CONNECTIONS <= CT_MAX_CONNS, "conn_track table too small");
static bool s_synced;
static gatt_event_cb_t s_cb;
static void *s_cb_ctx;
static bool s_adv_ready;  // the advertising event is initialised

static int gap_event(struct ble_gap_event *event, void *arg);

void gap_emit(gatt_event_t event, uint16_t conn_handle)
{
    if (s_cb != NULL) {
        s_cb(event, conn_handle, s_cb_ctx);
    }
}

// Advertising uses the extended advertising API (needed for extended
// scanning) with legacy PDUs, so phones see a plain ADV_IND as before.
#define ADV_INSTANCE 0

static struct ble_npl_event s_adv_ev;

void gap_adv_stop(void)
{
    // Sends the disable even when the host thinks the set is idle; EINVAL
    // only means it was never configured.
    ble_gap_ext_adv_stop(ADV_INSTANCE);
}

// Advertising or scan response data from fields (the mbuf is consumed).
static int set_adv_data(const struct ble_hs_adv_fields *fields, bool scan_rsp)
{
    struct os_mbuf *om = os_msys_get_pkthdr(BLE_HS_ADV_MAX_SZ, 0);
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }
    int rc = ble_hs_adv_set_fields_mbuf(fields, om);
    if (rc != 0) {
        os_mbuf_free_chain(om);
        return rc;
    }
    return scan_rsp ? ble_gap_ext_adv_rsp_set_data(ADV_INSTANCE, om) : ble_gap_ext_adv_set_data(ADV_INSTANCE, om);
}

// Starts, refreshes or stops advertising as the pairing policy says: one phone
// at a time, a second one only while the pairing window is open. Host task.
static void advertise_now(struct ble_npl_event *ev)
{
    (void)ev;
    if (!s_synced) {
        return;
    }
    gap_adv_stop();
    pp_adv_t mode = pairing_advertising();
    if (!mode.on) {
        return;
    }
    uint8_t flags = mode.flags;  // PP_ADV_FLAG_PAIRABLE, PP_ADV_FLAG_HAS_BOND
    if (board_gps_present()) {
        flags |= ADV_FLAG_GPS;
    }
    if (board_has_5ghz()) {
        flags |= ADV_FLAG_5GHZ;
    }
    const uint8_t mfg[5] = {
        COMPANY_ID & 0xFF, COMPANY_ID >> 8, ns_board_v1_ProtocolVersion_PROTOCOL_VERSION_MAJOR, flags,
        (uint8_t)board_model(),
    };

    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = &gatt_svc_uuid;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    adv.mfg_data = mfg;
    adv.mfg_data_len = sizeof(mfg);

    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;

    // Connectable and scannable undirected legacy advertising (ADV_IND).
    struct ble_gap_ext_adv_params params = {
        .connectable = 1,
        .scannable = 1,
        .legacy_pdu = 1,
        .itvl_min = ADV_ITVL_MIN,
        .itvl_max = ADV_ITVL_MAX,
        .own_addr_type = s_own_addr_type,
        .primary_phy = BLE_HCI_LE_PHY_1M,
        .secondary_phy = BLE_HCI_LE_PHY_1M,
        .tx_power = 127,  // controller default
    };
    int rc = ble_gap_ext_adv_configure(ADV_INSTANCE, &params, NULL, gap_event, NULL);
    if (rc == 0 && s_own_addr_type == BLE_OWN_ADDR_RANDOM) {
        ble_addr_t addr = {.type = BLE_ADDR_RANDOM};
        rc = ble_hs_id_copy_addr(BLE_ADDR_RANDOM, addr.val, NULL);
        if (rc == 0) {
            rc = ble_gap_ext_adv_set_addr(ADV_INSTANCE, &addr);
        }
    }
    if (rc == 0) {
        rc = set_adv_data(&adv, false);
    }
    if (rc == 0) {
        rc = set_adv_data(&rsp, true);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising setup: rc=%d", rc);
        return;
    }
    rc = ble_gap_ext_adv_start(ADV_INSTANCE, 0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising start: rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as %s (flags 0x%02x)", s_name, flags);
}

// Runs advertise_now from the host queue, after the event being handled. With
// extended advertising a connection arrives as CONNECT and then ADV_COMPLETE
// for the same set within one HCI event; refreshing in between would leave
// the host and controller disagreeing about the set's state.
void gap_advertise(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_adv_ev);
}

void gatt_server_refresh_advertising(void)
{
    if (s_adv_ready) {
        gap_advertise();  // safe from any task: only posts to the host queue
    }
}

static void request_link_params(uint16_t conn)
{
    int rc = ble_gap_set_prefered_le_phy(conn, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK,
                                         BLE_GAP_LE_PHY_CODED_ANY);
    if (rc != 0) {
        ESP_LOGW(TAG, "2M PHY request: rc=%d", rc);
    }
    rc = ble_gap_set_data_len(conn, DLE_TX_OCTETS, DLE_TX_TIME_US);
    if (rc != 0) {
        ESP_LOGW(TAG, "data length request: rc=%d", rc);
    }
    // ATT MTU: the preferred MTU (CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU) is
    // offered when the phone, as GATT client, starts the exchange.
}

static void log_security(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) == 0) {
        ESP_LOGI(TAG, "security: encrypted=%d authenticated=%d bonded=%d key_size=%d",
                 desc.sec_state.encrypted, desc.sec_state.authenticated, desc.sec_state.bonded,
                 desc.sec_state.key_size);
    }
}

// Reports conn as connected at its first event: NimBLE may deliver
// ENC_CHANGE and SUBSCRIBE before CONNECT (conn_track.h).
static void conn_seen(uint16_t conn, const char *event)
{
    if (!ct_seen(&s_conns, conn)) {
        return;
    }
    s_conn_count++;
    ESP_LOGI(TAG, "connected, handle %d (%d connected, at %s)", conn, s_conn_count, event);
    request_link_params(conn);
    if (gatt_link_hooks != NULL) {
        gatt_link_hooks->connected(conn);
    }
    gap_emit(GATT_EVENT_CONNECTED, conn);
    pairing_on_connect(conn);  // advertising continues only in a window
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: status=%d", event->connect.status);
            gap_advertise();
            return 0;
        }
        conn_seen(event->connect.conn_handle, "connect");
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason 0x%03x", event->disconnect.reason);
        if (!ct_gone(&s_conns, event->disconnect.conn.conn_handle)) {
            gap_advertise();  // never reported: nobody else knows it
            return 0;
        }
        if (s_conn_count > 0) {
            s_conn_count--;
        }
        if (gatt_link_hooks != NULL) {
            gatt_link_hooks->disconnected(event->disconnect.conn.conn_handle);
        }
        gap_emit(GATT_EVENT_DISCONNECTED, event->disconnect.conn.conn_handle);
        pairing_on_disconnect(event->disconnect.conn.conn_handle);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        gap_advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        conn_seen(event->enc_change.conn_handle, "encryption");
        ESP_LOGI(TAG, "encryption change: status=%d", event->enc_change.status);
        log_security(event->enc_change.conn_handle);
        pairing_on_enc_change(event->enc_change.conn_handle, event->enc_change.status);
        if (event->enc_change.status == 0) {
            gap_emit(GATT_EVENT_ENCRYPTED, event->enc_change.conn_handle);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        conn_seen(event->repeat_pairing.conn_handle, "repeat pairing");
        return pairing_on_repeat_pairing(event->repeat_pairing.conn_handle);

    case BLE_GAP_EVENT_AUTHORIZE:
        // CCCD writes of the NS Board Service characteristics on an encrypted
        // link (NOTIFY_INDICATE_AUTHOR): only a bonded phone may subscribe.
        event->authorize.out_response = gatt_link_bonded(event->authorize.conn_handle)
                                            ? BLE_GAP_AUTHORIZE_ACCEPT
                                            : BLE_GAP_AUTHORIZE_REJECT;
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE: {
        conn_seen(event->subscribe.conn_handle, "subscribe");
        ESP_LOGI(TAG, "subscribe: attr %d notify=%d indicate=%d", event->subscribe.attr_handle,
                 event->subscribe.cur_notify, event->subscribe.cur_indicate);
        // Only the NS Board Service characteristics are bonded-only. Phones
        // subscribe to Service Changed before pairing, which is fine.
        gatt_chr_t chr = gatt_chr_from_handle(event->subscribe.attr_handle);
        if (chr == GATT_CHR_COUNT) {
            return 0;
        }
        bool enabled = event->subscribe.cur_notify || event->subscribe.cur_indicate;
        if (enabled && !gatt_link_bonded(event->subscribe.conn_handle)) {
            // The CCCD write is refused before this (AUTHORIZE above); this
            // only guards against a subscription restored for a stale bond.
            ESP_LOGW(TAG, "subscribe on an unbonded link, ignored");
            return 0;
        }
        if (gatt_link_hooks != NULL) {
            gatt_link_hooks->subscribed(event->subscribe.conn_handle, chr, enabled);
        }
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_TX:
        // Indications report twice: 0 when queued, then EDONE (confirmed),
        // ETIMEOUT or another error. Notifications report their result to the
        // caller of gatt_server_notify directly.
        if (event->notify_tx.indication && event->notify_tx.status != 0 && gatt_link_hooks != NULL) {
            gatt_chr_t chr = gatt_chr_from_handle(event->notify_tx.attr_handle);
            if (chr != GATT_CHR_COUNT) {
                gatt_link_hooks->indicate_done(event->notify_tx.conn_handle, chr, event->notify_tx.status);
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "PHY tx=%d rx=%d (status %d)", event->phy_updated.tx_phy, event->phy_updated.rx_phy,
                 event->phy_updated.status);
        return 0;

    case BLE_GAP_EVENT_DATA_LEN_CHG:
        ESP_LOGI(TAG, "data length tx=%d rx=%d", event->data_len_chg.max_tx_octets,
                 event->data_len_chg.max_rx_octets);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "connection parameters updated (status %d)", event->conn_update.status);
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    uint8_t addr[6] = {0};
    if (rc == 0) {
        rc = ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "no BLE identity address: rc=%d", rc);
        return;
    }
    // NimBLE stores addresses least significant byte first.
    for (int i = 0; i < 6; i++) {
        gatt_bt_address[i] = addr[5 - i];
    }
    snprintf(gatt_short_id, sizeof(gatt_short_id), "%02X%02X", addr[1], addr[0]);
    snprintf(s_name, sizeof(s_name), "NS-%s", gatt_short_id);
    ble_svc_gap_device_name_set(s_name);
    ESP_LOGI(TAG, "BLE address %02X:%02X:%02X:%02X:%02X:%02X, name %s", gatt_bt_address[0],
             gatt_bt_address[1], gatt_bt_address[2], gatt_bt_address[3], gatt_bt_address[4],
             gatt_bt_address[5], s_name);

    ble_gap_set_prefered_default_le_phy(BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK);
    ble_gap_write_sugg_def_data_len(DLE_TX_OCTETS, DLE_TX_TIME_US);

    s_synced = true;
    pairing_on_sync();  // counts bonds and advertises
}

static void on_reset(int reason)
{
    s_synced = false;
    // The host forgets its connections without reporting their ends.
    ct_init(&s_conns);
    s_conn_count = 0;
    pairing_on_reset();
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t gatt_server_init(gatt_event_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_npl_event_init(&s_adv_ev, advertise_now, NULL);
    s_adv_ready = true;

    // LE Secure Connections, Just Works, bonding, encryption and identity keys
    // both ways (the phone's IRK lets the board resolve its private address).
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    int rc = gatt_svc_register();
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration: rc=%d", rc);
        return ESP_FAIL;
    }
    ble_store_config_init();  // bonds in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST)
    pairing_init();           // bond gate on top of the store

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

const char *gatt_server_short_id(void)
{
    return gatt_short_id;
}

int gatt_server_connection_count(void)
{
    return s_conn_count;
}
