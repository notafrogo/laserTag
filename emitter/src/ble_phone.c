#include "ble_phone.h"
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_phone, LOG_LEVEL_INF);

/* Emitter Service: e4b50001-c812-4d2b-b6d0-456789abcdef */
#define BT_UUID_EMITTER_SVC_VAL BT_UUID_128_ENCODE(0xe4b50001, 0xc812, 0x4d2b, 0xb6d0, 0x456789abcdef)
#define BT_UUID_EMITTER_RX_VAL  BT_UUID_128_ENCODE(0xe4b50002, 0xc812, 0x4d2b, 0xb6d0, 0x456789abcdef)
#define BT_UUID_EMITTER_TX_VAL  BT_UUID_128_ENCODE(0xe4b50003, 0xc812, 0x4d2b, 0xb6d0, 0x456789abcdef)

#define BT_UUID_EMITTER_SVC BT_UUID_DECLARE_128(BT_UUID_EMITTER_SVC_VAL)
#define BT_UUID_EMITTER_RX  BT_UUID_DECLARE_128(BT_UUID_EMITTER_RX_VAL)
#define BT_UUID_EMITTER_TX  BT_UUID_DECLARE_128(BT_UUID_EMITTER_TX_VAL)

static ble_phone_rx_cb_t app_rx_cb;

static ssize_t on_rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len > 0 && app_rx_cb) {
        app_rx_cb((const uint8_t *)buf, len);
    }
    return len;
}

BT_GATT_SERVICE_DEFINE(emitter_phone_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_EMITTER_SVC),
    BT_GATT_CHARACTERISTIC(BT_UUID_EMITTER_TX,
                           BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE,
                           NULL, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(BT_UUID_EMITTER_RX,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE,
                           NULL, on_rx_write, NULL),
);

void ble_phone_init(ble_phone_rx_cb_t rx_cb)
{
    app_rx_cb = rx_cb;
}

void ble_phone_notify(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
    if (!conn) {
        return;
    }
    int err = bt_gatt_notify(conn, &emitter_phone_svc.attrs[2], data, len);
    if (err) {
        LOG_WRN("Phone notify failed (err %d)", err);
    }
}
