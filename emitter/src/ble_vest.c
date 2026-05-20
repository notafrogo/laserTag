#include "ble_vest.h"
#include "protocol.h"
#include "mesh.h"
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ble_vest, LOG_LEVEL_INF);

/* Vest Service UUIDs */
#define BT_UUID_VEST_SVC_VAL    BT_UUID_128_ENCODE(0xa7d80001, 0x1234, 0x5678, 0xabcd, 0x0123456789ab)
#define BT_UUID_VEST_HIT_TX_VAL BT_UUID_128_ENCODE(0xa7d80002, 0x1234, 0x5678, 0xabcd, 0x0123456789ab)
#define BT_UUID_VEST_CMD_RX_VAL BT_UUID_128_ENCODE(0xa7d80003, 0x1234, 0x5678, 0xabcd, 0x0123456789ab)

static struct bt_uuid_128 vest_svc_uuid    = BT_UUID_INIT_128(BT_UUID_VEST_SVC_VAL);
static struct bt_uuid_128 vest_hit_tx_uuid = BT_UUID_INIT_128(BT_UUID_VEST_HIT_TX_VAL);
static struct bt_uuid_128 vest_cmd_rx_uuid = BT_UUID_INIT_128(BT_UUID_VEST_CMD_RX_VAL);

/* State */
static struct bt_conn *vest_conn;
static uint16_t cmd_rx_handle;
static ble_vest_hit_cb_t hit_cb;
static ble_vest_connected_cb_t connected_cb;
static ble_vest_disconnected_cb_t disconnected_cb;
static uint8_t vest_mac[6];
static bool has_vest_mac;

/* GATT discovery state machine */
enum discover_stage {
    DISC_SERVICE,
    DISC_CHAR,
    DISC_CCC,
    DISC_DONE,
};
static enum discover_stage disc_stage;
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_subscribe_params subscribe_params;
static uint16_t svc_start_handle;
static uint16_t svc_end_handle;
static uint16_t hit_tx_value_handle;

/* Pairing broadcast */
static struct bt_le_ext_adv *pairing_adv_set;
static struct k_work_delayable pairing_stop_work;

/* Forward declarations */
static uint8_t gatt_discover_cb(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                struct bt_gatt_discover_params *params);

static uint8_t hit_notify_cb(struct bt_conn *conn,
                             struct bt_gatt_subscribe_params *params,
                             const void *data, uint16_t length)
{
    if (!data || length < VEST_HIT_PAYLOAD_SIZE) {
        return BT_GATT_ITER_CONTINUE;
    }

    const uint8_t *d = data;
    uint16_t packet_id = sys_get_le16(&d[0]);
    uint8_t shooter_id = d[2];
    uint8_t weapon_id  = d[3];
    uint8_t team_id    = d[4];

    LOG_INF("Vest hit: pkt=0x%04x shooter=%d weapon=%d team=%d",
            packet_id, shooter_id, weapon_id, team_id);

    if (hit_cb) {
        hit_cb(packet_id, shooter_id, weapon_id, team_id);
    }
    return BT_GATT_ITER_CONTINUE;
}

static void start_gatt_discovery(struct bt_conn *conn)
{
    disc_stage = DISC_SERVICE;
    discover_params.uuid = &vest_svc_uuid.uuid;
    discover_params.func = gatt_discover_cb;
    discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    discover_params.type = BT_GATT_DISCOVER_PRIMARY;

    int err = bt_gatt_discover(conn, &discover_params);
    if (err) {
        LOG_ERR("GATT discovery start failed (err %d)", err);
    }
}

static uint8_t gatt_discover_cb(struct bt_conn *conn,
                                const struct bt_gatt_attr *attr,
                                struct bt_gatt_discover_params *params)
{
    if (!attr) {
        if (disc_stage == DISC_SERVICE) {
            LOG_ERR("Vest service not found");
        }
        return BT_GATT_ITER_STOP;
    }

    switch (disc_stage) {
    case DISC_SERVICE: {
        struct bt_gatt_service_val *svc = attr->user_data;
        svc_start_handle = attr->handle + 1;
        svc_end_handle = svc->end_handle;
        LOG_INF("Vest service found: handles %d-%d", svc_start_handle, svc_end_handle);

        disc_stage = DISC_CHAR;
        discover_params.uuid = NULL;
        discover_params.start_handle = svc_start_handle;
        discover_params.end_handle = svc_end_handle;
        discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

        int err = bt_gatt_discover(conn, &discover_params);
        if (err) {
            LOG_ERR("Char discovery failed (err %d)", err);
        }
        return BT_GATT_ITER_STOP;
    }

    case DISC_CHAR: {
        struct bt_gatt_chrc *chrc = attr->user_data;
        if (!bt_uuid_cmp(chrc->uuid, &vest_hit_tx_uuid.uuid)) {
            hit_tx_value_handle = chrc->value_handle;
            LOG_INF("Hit TX char found: handle %d", hit_tx_value_handle);
        } else if (!bt_uuid_cmp(chrc->uuid, &vest_cmd_rx_uuid.uuid)) {
            cmd_rx_handle = chrc->value_handle;
            LOG_INF("Cmd RX char found: handle %d", cmd_rx_handle);
        }

        if (hit_tx_value_handle && cmd_rx_handle) {
            disc_stage = DISC_CCC;
            subscribe_params.notify = hit_notify_cb;
            subscribe_params.value_handle = hit_tx_value_handle;
            subscribe_params.ccc_handle = 0;
            subscribe_params.end_handle = svc_end_handle;
            subscribe_params.disc_params = &discover_params;
            subscribe_params.value = BT_GATT_CCC_NOTIFY;

            int err = bt_gatt_subscribe(conn, &subscribe_params);
            if (err && err != -EALREADY) {
                LOG_ERR("Subscribe failed (err %d)", err);
            } else {
                LOG_INF("Subscribed to vest Hit TX");
                disc_stage = DISC_DONE;
            }
            return BT_GATT_ITER_STOP;
        }
        return BT_GATT_ITER_CONTINUE;
    }

    default:
        return BT_GATT_ITER_STOP;
    }
}

static void vest_conn_connected(struct bt_conn *conn, uint8_t err)
{
    /* Bail early if this isn't our pending vest connection — the connected
     * callback also fires for phone connections, and an err here for the
     * phone must not wipe out a separately-tracked vest connection.
     */
    if (conn != vest_conn) {
        return;
    }

    if (err) {
        LOG_ERR("Vest connection failed (err %d)", err);
        bt_conn_unref(vest_conn);
        vest_conn = NULL;
        mesh_start_scanner();
        return;
    }

    const bt_addr_le_t *addr = bt_conn_get_dst(conn);
    if (!addr) {
        return;
    }

    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    LOG_INF("Vest connected: %s", addr_str);

    memcpy(vest_mac, addr->a.val, 6);
    has_vest_mac = true;

    cmd_rx_handle = 0;
    hit_tx_value_handle = 0;
    start_gatt_discovery(conn);

    mesh_start_scanner();

    if (connected_cb) {
        connected_cb(vest_mac);
    }
}

static void vest_conn_disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (conn == vest_conn) {
        LOG_INF("Vest disconnected (reason %d)", reason);
        bt_conn_unref(vest_conn);
        vest_conn = NULL;
        cmd_rx_handle = 0;
        hit_tx_value_handle = 0;

        mesh_start_scanner();

        if (disconnected_cb) {
            disconnected_cb();
        }
    }
}

BT_CONN_CB_DEFINE(vest_conn_cbs) = {
    .connected = vest_conn_connected,
    .disconnected = vest_conn_disconnected,
};

/* ===== Vest Scan (1M PHY for vest pairing/reconnect) ===== */

void ble_vest_stop_scan(void)
{
    int err = bt_le_scan_stop();
    if (err && err != -EALREADY) {
        LOG_WRN("Stop scan failed (err %d)", err);
    }
}

static void vest_scan_connect(const bt_addr_le_t *addr)
{
    if (vest_conn) {
        mesh_start_scanner();
        return;
    }

    struct bt_le_conn_param conn_param = BT_LE_CONN_PARAM_INIT(
        BT_GAP_INIT_CONN_INT_MIN, BT_GAP_INIT_CONN_INT_MAX, 0, 400);

    struct bt_conn *conn;
    int err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN,
                                &conn_param, &conn);
    if (err) {
        LOG_ERR("Vest connect failed (err %d)", err);
        mesh_start_scanner();
        return;
    }
    vest_conn = conn;
    LOG_INF("Connecting to vest...");
}

/* ===== Pairing Broadcast ===== */

static void pairing_stop_handler(struct k_work *work)
{
    if (pairing_adv_set) {
        bt_le_ext_adv_stop(pairing_adv_set);
        LOG_INF("Pairing broadcast stopped");
    }
}

void ble_vest_init(ble_vest_hit_cb_t hit_callback,
                   ble_vest_connected_cb_t conn_cb,
                   ble_vest_disconnected_cb_t disc_cb)
{
    hit_cb = hit_callback;
    connected_cb = conn_cb;
    disconnected_cb = disc_cb;
    k_work_init_delayable(&pairing_stop_work, pairing_stop_handler);
}

void ble_vest_start_pairing_broadcast(const uint8_t *emitter_mac, uint8_t player_id)
{
    if (!pairing_adv_set) {
        struct bt_le_adv_param param = {
            .id = BT_ID_DEFAULT,
            .sid = 2,
            .secondary_max_skip = 0,
            .options = BT_LE_ADV_OPT_EXT_ADV,
            .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
            .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
            .peer = NULL,
        };
        int err = bt_le_ext_adv_create(&param, NULL, &pairing_adv_set);
        if (err) {
            LOG_ERR("Pairing adv set create failed (err %d)", err);
            return;
        }
    }

    uint8_t pairing_data[PAIRING_ADV_SIZE];
    pairing_data[0] = PAIRING_FLAG;
    memcpy(&pairing_data[1], emitter_mac, 6);
    pairing_data[7] = player_id;

    struct bt_data ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA, pairing_data, PAIRING_ADV_SIZE),
    };

    int err = bt_le_ext_adv_set_data(pairing_adv_set, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("Pairing adv set data failed (err %d)", err);
        return;
    }

    struct bt_le_ext_adv_start_param start = { .timeout = 200, .num_events = 0 };
    err = bt_le_ext_adv_start(pairing_adv_set, &start);
    if (err) {
        LOG_ERR("Pairing adv start failed (err %d)", err);
        return;
    }

    LOG_INF("Pairing broadcast started (~2s)");
    k_work_reschedule(&pairing_stop_work, K_MSEC(2000));
}

void ble_vest_on_scan_result(const bt_addr_le_t *addr, int8_t rssi,
                             struct net_buf_simple *buf)
{
    if (vest_conn) {
        return;
    }

    /* Look for vest name prefix "LT-V-" in the advertisement */
    if (buf->len >= 2) {
        uint8_t *d = buf->data;
        uint16_t pos = 0;
        while (pos + 1 < buf->len) {
            uint8_t ad_len = d[pos];
            if (ad_len == 0 || pos + 1 + ad_len > buf->len) {
                break;
            }
            uint8_t ad_type = d[pos + 1];
            if ((ad_type == BT_DATA_NAME_COMPLETE || ad_type == BT_DATA_NAME_SHORTENED) &&
                ad_len >= 6) {
                const char *name = (const char *)&d[pos + 2];
                if (name[0] == 'L' && name[1] == 'T' && name[2] == '-' &&
                    name[3] == 'V' && name[4] == '-') {
                    LOG_INF("Found vest: RSSI=%d", rssi);
                    ble_vest_stop_scan();
                    vest_scan_connect(addr);
                    return;
                }
            }
            pos += ad_len + 1;
        }
    }
}

/* ===== Commands to Vest ===== */

static int vest_write(const uint8_t *data, uint16_t len)
{
    if (!vest_conn || cmd_rx_handle == 0) {
        return -ENOTCONN;
    }
    return bt_gatt_write_without_resp(vest_conn, cmd_rx_handle, data, len, false);
}

int ble_vest_send_hit_ack(uint16_t packet_id)
{
    uint8_t buf[3];
    buf[0] = VEST_CMD_HIT_ACK;
    sys_put_le16(packet_id, &buf[1]);
    return vest_write(buf, 3);
}

int ble_vest_send_friendly_list(const uint8_t *player_ids, uint8_t count)
{
    uint8_t buf[2 + MAX_PLAYERS];
    buf[0] = VEST_CMD_FRIENDLY_LIST;
    buf[1] = count;
    if (count > MAX_PLAYERS) count = MAX_PLAYERS;
    memcpy(&buf[2], player_ids, count);
    return vest_write(buf, 2 + count);
}

int ble_vest_send_death(void)
{
    uint8_t buf[1] = { VEST_CMD_DEATH };
    return vest_write(buf, 1);
}

int ble_vest_send_respawn(void)
{
    uint8_t buf[1] = { VEST_CMD_RESPAWN };
    return vest_write(buf, 1);
}

int ble_vest_send_friendly_fire(uint8_t enabled)
{
    uint8_t buf[2] = { VEST_CMD_FRIENDLY_FIRE, enabled };
    return vest_write(buf, 2);
}

bool ble_vest_is_connected(void)
{
    return vest_conn != NULL && cmd_rx_handle != 0;
}

void ble_vest_disconnect(void)
{
    if (vest_conn) {
        bt_conn_disconnect(vest_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

bool ble_vest_get_active_mac(uint8_t mac_out[6])
{
    if (!vest_conn || !has_vest_mac) {
        return false;
    }
    memcpy(mac_out, vest_mac, 6);
    return true;
}
