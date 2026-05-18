#ifndef BLE_PHONE_H
#define BLE_PHONE_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <stdint.h>

typedef void (*ble_phone_rx_cb_t)(const uint8_t *data, uint16_t len);

void ble_phone_init(ble_phone_rx_cb_t rx_cb);
void ble_phone_notify(struct bt_conn *conn, const uint8_t *data, uint16_t len);

#endif
