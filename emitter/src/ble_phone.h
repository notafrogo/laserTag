#ifndef BLE_PHONE_H
#define BLE_PHONE_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <stdint.h>

typedef void (*ble_phone_rx_cb_t)(const uint8_t *data, uint16_t len);

void ble_phone_init(ble_phone_rx_cb_t rx_cb);
void ble_phone_notify(struct bt_conn *conn, const uint8_t *data, uint16_t len);

/* ===== Log relay to phone =====
 *
 * Forwards short log lines to the phone over the existing emitter
 * connection as RSP_LOG notifications. Useful when the emitter is on
 * battery and SWD isn't accessible: the iOS app's log viewer surfaces
 * what the RTT terminal would normally show.
 *
 * Severity values: 0=DBG 1=INF 2=WRN 3=ERR. Lines longer than the
 * negotiated ATT MTU are truncated (~150 bytes typically).
 *
 * Use PLOG_INF/PLOG_WRN/PLOG_ERR macros: they log to the Zephyr backend
 * (RTT) AND relay to the phone in one call, so there's a single source
 * of truth for the message text.
 */
void ble_phone_log_set_conn(struct bt_conn *conn);
void ble_phone_log(uint8_t severity, const char *fmt, ...);

#define PLOG_INF(fmt, ...) do { \
    LOG_INF(fmt, ##__VA_ARGS__); \
    ble_phone_log(1, fmt, ##__VA_ARGS__); \
} while (0)

#define PLOG_WRN(fmt, ...) do { \
    LOG_WRN(fmt, ##__VA_ARGS__); \
    ble_phone_log(2, fmt, ##__VA_ARGS__); \
} while (0)

#define PLOG_ERR(fmt, ...) do { \
    LOG_ERR(fmt, ##__VA_ARGS__); \
    ble_phone_log(3, fmt, ##__VA_ARGS__); \
} while (0)

#endif
