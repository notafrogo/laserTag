#include "vest_ble.h"
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(vest_ble, LOG_LEVEL_INF);

/* Vest Service: a7d80001-1234-5678-abcd-0123456789ab */
#define BT_UUID_VEST_SVC_VAL    BT_UUID_128_ENCODE(0xa7d80001, 0x1234, 0x5678, 0xabcd, 0x0123456789ab)
#define BT_UUID_VEST_HIT_TX_VAL BT_UUID_128_ENCODE(0xa7d80002, 0x1234, 0x5678, 0xabcd, 0x0123456789ab)
#define BT_UUID_VEST_CMD_RX_VAL BT_UUID_128_ENCODE(0xa7d80003, 0x1234, 0x5678, 0xabcd, 0x0123456789ab)

#define BT_UUID_VEST_SVC    BT_UUID_DECLARE_128(BT_UUID_VEST_SVC_VAL)
#define BT_UUID_VEST_HIT_TX BT_UUID_DECLARE_128(BT_UUID_VEST_HIT_TX_VAL)
#define BT_UUID_VEST_CMD_RX BT_UUID_DECLARE_128(BT_UUID_VEST_CMD_RX_VAL)

static vest_ble_cmd_cb_t app_cmd_cb;

static ssize_t on_cmd_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len > 0 && app_cmd_cb) {
        app_cmd_cb((const uint8_t *)buf, len);
    }
    return len;
}

BT_GATT_SERVICE_DEFINE(vest_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_VEST_SVC),
    /* Hit TX: vest -> emitter (notify) */
    BT_GATT_CHARACTERISTIC(BT_UUID_VEST_HIT_TX,
                           BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE,
                           NULL, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    /* Command RX: emitter -> vest (write-with-response) */
    BT_GATT_CHARACTERISTIC(BT_UUID_VEST_CMD_RX,
                           BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,
                           NULL, on_cmd_write, NULL),
);

void vest_ble_init(vest_ble_cmd_cb_t cmd_cb)
{
    app_cmd_cb = cmd_cb;
}

int vest_ble_notify_hit(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
    if (!conn) {
        return -ENOTCONN;
    }
    return bt_gatt_notify(conn, &vest_svc.attrs[2], data, len);
}
