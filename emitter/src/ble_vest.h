#ifndef BLE_VEST_H
#define BLE_VEST_H

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/net_buf.h>
#include <stdint.h>
#include <stdbool.h>

typedef void (*ble_vest_hit_cb_t)(uint16_t packet_id, uint8_t shooter_id,
                                  uint8_t weapon_id, uint8_t team_id);
typedef void (*ble_vest_connected_cb_t)(const uint8_t *vest_mac);
typedef void (*ble_vest_disconnected_cb_t)(uint8_t reason);
/* Fired once GATT discovery + subscription have completed and writes to
 * the vest's command characteristic will succeed. Used by the emitter to
 * send VEST_CMD_PAIR_CONFIRM at the right moment.
 */
typedef void (*ble_vest_ready_cb_t)(void);

void ble_vest_init(ble_vest_hit_cb_t hit_cb,
                   ble_vest_connected_cb_t connected_cb,
                   ble_vest_disconnected_cb_t disconnected_cb,
                   ble_vest_ready_cb_t ready_cb);

/* Called from main scan callback when a 1M advertisement is received */
void ble_vest_on_scan_result(const bt_addr_le_t *addr, int8_t rssi,
                             struct net_buf_simple *buf);

void ble_vest_stop_scan(void);

int ble_vest_send_hit_ack(uint16_t packet_id);
int ble_vest_send_friendly_list(const uint8_t *player_ids, uint8_t count);
int ble_vest_send_death(void);
int ble_vest_send_respawn(void);
int ble_vest_send_friendly_fire(uint8_t enabled);
int ble_vest_send_pair_confirm(uint16_t nonce);
int ble_vest_send_unpair(void);

bool ble_vest_is_connected(void);
void ble_vest_disconnect(void);

/* If a vest is currently connected at BLE level, copy its 6-byte MAC into
 * mac_out and return true.
 */
bool ble_vest_get_active_mac(uint8_t mac_out[6]);

#endif
