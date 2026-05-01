#ifndef BLE_GATT_H
#define BLE_GATT_H

#include <stdint.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>

/* LaserTag Service UUIDs */
#if defined(LASERTAG_IS_RECEIVER)
    /* Receiver Service UUID: f5c60001-d923-5e3c-c7e1-56789abcdeff */
    #define BT_UUID_LASERTAG_SERVICE_VAL BT_UUID_128_ENCODE(0xf5c60001, 0xd923, 0x5e3c, 0xc7e1, 0x56789abcdeff)
    #define BT_UUID_LASERTAG_RX_CHAR_VAL BT_UUID_128_ENCODE(0xf5c60002, 0xd923, 0x5e3c, 0xc7e1, 0x56789abcdeff)
    #define BT_UUID_LASERTAG_TX_CHAR_VAL BT_UUID_128_ENCODE(0xf5c60003, 0xd923, 0x5e3c, 0xc7e1, 0x56789abcdeff)
#else
    /* Emitter Service UUID: e4b50001-c812-4d2b-b6d0-456789abcdef */
    #define BT_UUID_LASERTAG_SERVICE_VAL BT_UUID_128_ENCODE(0xe4b50001, 0xc812, 0x4d2b, 0xb6d0, 0x456789abcdef)
    #define BT_UUID_LASERTAG_RX_CHAR_VAL BT_UUID_128_ENCODE(0xe4b50002, 0xc812, 0x4d2b, 0xb6d0, 0x456789abcdef)
    #define BT_UUID_LASERTAG_TX_CHAR_VAL BT_UUID_128_ENCODE(0xe4b50003, 0xc812, 0x4d2b, 0xb6d0, 0x456789abcdef)
#endif

#define BT_UUID_LASERTAG_SERVICE BT_UUID_DECLARE_128(BT_UUID_LASERTAG_SERVICE_VAL)
#define BT_UUID_LASERTAG_RX_CHAR BT_UUID_DECLARE_128(BT_UUID_LASERTAG_RX_CHAR_VAL)
#define BT_UUID_LASERTAG_TX_CHAR BT_UUID_DECLARE_128(BT_UUID_LASERTAG_TX_CHAR_VAL)

/* Callback for processing incoming commands from BLE GATT */
typedef void (*ble_gatt_rx_cb_t)(const uint8_t *data, uint16_t len);

/**
 * @brief Initialize the BLE GATT service handler logic with the given callback.
 *        This should be called before configuring connection handling or handling commands.
 * 
 * @param rx_cb Function to be called when a packet is written to the RX characteristic
 */
void ble_gatt_init(ble_gatt_rx_cb_t rx_cb);

/**
 * @brief Send a BLE GATT Notification using the TX characteristic
 * 
 * @param conn The bluetooth connection to notify on
 * @param data Payload
 * @param len Payload length
 */
void ble_gatt_notify(struct bt_conn *conn, const uint8_t *data, uint16_t len);

#endif /* BLE_GATT_H */