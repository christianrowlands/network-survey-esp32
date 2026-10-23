// GATT database: NS Board Service, Device Information, GAP and GATT services.
// Info is implemented here; writes to Control and OTA, and every notification
// and indication, go through the link layer interface in gatt_server.h.

#include <string.h>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "gatt_internal.h"
#include "host/ble_hs.h"
#include "ns_board.pb.h"
#include "pb_encode.h"
#include "sdkconfig.h"
#include "services/dis/ble_svc_dis.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "gatt_svc";

#define CONTROL_MAX_WRITE 512
#define WRITE_MAX (BLE_ATT_MTU_MAX - 3)

const ble_uuid128_t gatt_svc_uuid = NS_UUID(0x0001);
static const ble_uuid128_t s_info_uuid = NS_UUID(0x0002);
static const ble_uuid128_t s_control_uuid = NS_UUID(0x0003);
static const ble_uuid128_t s_status_uuid = NS_UUID(0x0004);
static const ble_uuid128_t s_records_uuid = NS_UUID(0x0005);
static const ble_uuid128_t s_raw_uuid = NS_UUID(0x0006);
static const ble_uuid128_t s_ota_uuid = NS_UUID(0x0007);

uint16_t gatt_chr_handles[GATT_CHR_COUNT];
static uint16_t s_info_handle;
const gatt_link_hooks_t *gatt_link_hooks;

static int encode_board_info(struct os_mbuf *om)
{
    ns_board_v1_BoardInfo info = ns_board_v1_BoardInfo_init_zero;
    info.protocol_major = ns_board_v1_ProtocolVersion_PROTOCOL_VERSION_MAJOR;
    info.protocol_minor = ns_board_v1_ProtocolVersion_PROTOCOL_VERSION_MINOR;
    strlcpy(info.firmware_version, esp_app_get_description()->version, sizeof(info.firmware_version));
    info.board_model = board_model();
    info.chip = board_chip();
    info.bands[info.bands_count++] = ns_board_v1_Band_BAND_GHZ_2_4;
    if (board_has_5ghz()) {
        info.bands[info.bands_count++] = ns_board_v1_Band_BAND_GHZ_5;
    }
    info.gps_present = board_gps_present();
    strlcpy(info.short_id, gatt_short_id, sizeof(info.short_id));
    info.preferred_mtu = CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU;
    memcpy(info.bt_address, gatt_bt_address, sizeof(info.bt_address));
    strlcpy(info.model_name, board_model_name(), sizeof(info.model_name));

    uint8_t buf[ns_board_v1_BoardInfo_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, ns_board_v1_BoardInfo_fields, &info)) {
        ESP_LOGE(TAG, "BoardInfo encode: %s", PB_GET_ERROR(&stream));
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(om, buf, stream.bytes_written) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int info_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return encode_board_info(ctxt->om);
}

// Control, Status, Records, Raw and OTA. NimBLE enforces encryption through
// the *_ENC flags; the bond check here turns away encrypted but unbonded links
// (their CCCD writes are refused through the AUTHOR flag and
// BLE_GAP_EVENT_AUTHORIZE in gap.c). Writes (Control and OTA) go to the link
// layer.
static int secured_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    gatt_chr_t chr = (gatt_chr_t)(intptr_t)arg;
    if (!gatt_link_bonded(conn_handle)) {
        // protocol.md section 2: 0x05 on an unencrypted link (the phone then
        // pairs), 0x08 when encrypted without a stored bond.
        struct ble_gap_conn_desc desc;
        bool encrypted = ble_gap_conn_find(conn_handle, &desc) == 0 && desc.sec_state.encrypted;
        return encrypted ? BLE_ATT_ERR_INSUFFICIENT_AUTHOR : BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    // Access callbacks all run in the host task, so one buffer is enough.
    static uint8_t buf[WRITE_MAX];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if ((chr == GATT_CHR_CONTROL && len > CONTROL_MAX_WRITE) || len > sizeof(buf)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (gatt_link_hooks == NULL || gatt_link_hooks->write == NULL) {
        ESP_LOGW(TAG, "write chr %d, %u bytes: no handler", chr, len);
        return 0;
    }
    return gatt_link_hooks->write(conn_handle, chr, buf, len);
}

// CCCD writes need encryption (an unencrypted write makes the phone pair) and
// then pass the bond check in BLE_GAP_EVENT_AUTHORIZE.
#define NOTIFY_SECURED (BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR)

#define SECURED_NOTIFY_CHR(id, idx)                                                               \
    {                                                                                               \
        .uuid = &(id).u, .access_cb = secured_access, .arg = (void *)(intptr_t)(idx),               \
        .flags = BLE_GATT_CHR_F_NOTIFY | NOTIFY_SECURED,                                            \
        .val_handle = &gatt_chr_handles[idx],                                                       \
    }

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_info_uuid.u,
                .access_cb = info_access,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = &s_info_handle,
            },
            {
                .uuid = &s_control_uuid.u,
                .access_cb = secured_access,
                .arg = (void *)(intptr_t)GATT_CHR_CONTROL,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_INDICATE | NOTIFY_SECURED,
                .val_handle = &gatt_chr_handles[GATT_CHR_CONTROL],
            },
            SECURED_NOTIFY_CHR(s_status_uuid, GATT_CHR_STATUS),
            SECURED_NOTIFY_CHR(s_records_uuid, GATT_CHR_RECORDS),
            SECURED_NOTIFY_CHR(s_raw_uuid, GATT_CHR_RAW),
            {
                .uuid = &s_ota_uuid.u,
                .access_cb = secured_access,
                .arg = (void *)(intptr_t)GATT_CHR_OTA,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_NOTIFY |
                         NOTIFY_SECURED,
                .val_handle = &gatt_chr_handles[GATT_CHR_OTA],
            },
            {0},
        },
    },
    {0},
};

int gatt_svc_register(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();  // includes Service Changed

    ble_svc_dis_init();  // allocates the DIS values, so set them after
    ble_svc_dis_manufacturer_name_set("Network Survey");
    ble_svc_dis_model_number_set(board_model_name());
    ble_svc_dis_hardware_revision_set(board_chip_name());
    ble_svc_dis_firmware_revision_set(esp_app_get_description()->version);

    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    return rc;
}

gatt_chr_t gatt_chr_from_handle(uint16_t attr_handle)
{
    for (int i = 0; i < GATT_CHR_COUNT; i++) {
        if (gatt_chr_handles[i] == attr_handle) {
            return (gatt_chr_t)i;
        }
    }
    return GATT_CHR_COUNT;
}

void gatt_server_set_link_hooks(const gatt_link_hooks_t *hooks)
{
    gatt_link_hooks = hooks;
}

static int send_value(uint16_t conn, gatt_chr_t chr, const uint8_t *data, uint16_t len, bool indicate)
{
    if (chr >= GATT_CHR_COUNT) {
        return BLE_HS_EINVAL;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }
    // Both consume om whatever the outcome.
    if (indicate) {
        return ble_gatts_indicate_custom(conn, gatt_chr_handles[chr], om);
    }
    return ble_gatts_notify_custom(conn, gatt_chr_handles[chr], om);
}

int gatt_server_notify(uint16_t conn, gatt_chr_t chr, const uint8_t *data, uint16_t len)
{
    return send_value(conn, chr, data, len, false);
}

int gatt_server_indicate(uint16_t conn, gatt_chr_t chr, const uint8_t *data, uint16_t len)
{
    return send_value(conn, chr, data, len, true);
}

uint16_t gatt_server_mtu(uint16_t conn)
{
    return ble_att_mtu(conn);
}

int gatt_server_tx_buffers_free(void)
{
    return os_msys_num_free();
}

void gatt_server_disconnect(uint16_t conn)
{
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}
