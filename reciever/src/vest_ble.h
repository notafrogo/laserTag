#ifndef VEST_BLE_H
#define VEST_BLE_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <stdint.h>
#include <stdbool.h>

/* Callback for commands received from emitter */
typedef void (*vest_ble_cmd_cb_t)(const uint8_t *data, uint16_t len);

void vest_ble_init(vest_ble_cmd_cb_t cmd_cb);

/* Send a hit notification to emitter, returns 0 on success */
int vest_ble_notify_hit(struct bt_conn *conn, const uint8_t *data, uint16_t len);

#endif
