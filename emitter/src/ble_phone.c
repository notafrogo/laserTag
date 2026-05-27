#include "ble_phone.h"
#include "protocol.h"
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(ble_phone, LOG_LEVEL_INF);

/* Emitter Service: e4b50001-c812-4d2b-b6d0-456789abcdef */
#define BT_UUID_EMITTER_SVC_VAL BT_UUID_128_ENCODE(0xe4b50001, 0xc812, 0x4d2b, 0xb6d0, 0x456789abcdef)
#define BT_UUID_EMITTER_RX_VAL  BT_UUID_128_ENCODE(0xe4b50002, 0xc812, 0x4d2b, 0xb6d0, 0x456789abcdef)
#define BT_UUID_EMITTER_TX_VAL  BT_UUID_128_ENCODE(0xe4b50003, 0xc812, 0x4d2b, 0xb6d0, 0x456789abcdef)

#define BT_UUID_EMITTER_SVC BT_UUID_DECLARE_128(BT_UUID_EMITTER_SVC_VAL)
#define BT_UUID_EMITTER_RX  BT_UUID_DECLARE_128(BT_UUID_EMITTER_RX_VAL)
#define BT_UUID_EMITTER_TX  BT_UUID_DECLARE_128(BT_UUID_EMITTER_TX_VAL)

static ble_phone_rx_cb_t app_rx_cb;

/* Tracked separately from the caller's `phone_conn` so the logging path
 * doesn't have to thread a connection pointer through every module.
 * Set/cleared by main.c via ble_phone_log_set_conn in the BLE
 * connected/disconnected callbacks.
 */
static struct bt_conn *log_conn;

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

void ble_phone_log_set_conn(struct bt_conn *conn)
{
    log_conn = conn;
}

void ble_phone_log(uint8_t severity, const char *fmt, ...)
{
    if (!log_conn) {
        return;
    }

    /* notify_buf layout: [opcode][severity][text...] — text is NOT
     * NUL-terminated; the receiver uses the BLE notify length to know
     * where it ends.
     */
    uint8_t notify_buf[152];
    notify_buf[0] = RSP_LOG;
    notify_buf[1] = severity;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf((char *)&notify_buf[2], sizeof(notify_buf) - 2, fmt, ap);
    va_end(ap);

    if (n < 0) {
        return;
    }
    size_t text_len = (size_t)n;
    if (text_len > sizeof(notify_buf) - 2) {
        text_len = sizeof(notify_buf) - 2;  /* vsnprintf truncated */
    }
    size_t total = 2 + text_len;

    /* Notification payload is bounded by the current ATT MTU minus the
     * 3-byte ATT_HANDLE_VALUE_NTF header. iOS auto-negotiates ~185 byte
     * MTU shortly after connect but the exchange takes a moment — if a
     * log line fires before MTU is up, bt_gatt_notify returns -EMSGSIZE
     * and the entire line is silently lost. Trim to whatever MTU
     * actually is right now (worst case 23 → 20 byte payloads).
     */
    uint16_t mtu = bt_gatt_get_mtu(log_conn);
    if (mtu < 5) {
        return;  /* shouldn't happen — bail rather than write a negative size */
    }
    size_t max_payload = (size_t)mtu - 3;
    if (total > max_payload) {
        total = max_payload;
    }

    /* bt_gatt_notify is non-blocking and returns -ENOMEM if the host's
     * TX queue is full; we drop the log line in that case rather than
     * blocking the caller (which may be on the BT host thread itself).
     * Log failures so they're at least visible in RTT when attached.
     */
    int err = bt_gatt_notify(log_conn, &emitter_phone_svc.attrs[2],
                             notify_buf, total);
    if (err) {
        LOG_WRN("RSP_LOG notify failed (err %d, mtu=%u, total=%u)",
                err, mtu, (unsigned)total);
    }
}
