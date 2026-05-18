#ifndef MESH_H
#define MESH_H

#include "protocol.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <stdint.h>

void mesh_init(void);
void mesh_set_identity(uint8_t player_id, uint8_t team_id);
void mesh_set_phone_conn(struct bt_conn *conn);
void mesh_start_scanner(void);
void mesh_stop_scanner(void);

/* Raw broadcast (for CMD_BROADCAST_CODED_PHY from phone) */
void mesh_broadcast_raw(const uint8_t *data, uint16_t len);

/* Typed broadcasts */
void mesh_broadcast_lobby_announce(uint32_t lobby_code, uint8_t game_mode,
                                   const char *host_name, uint8_t player_count);
void mesh_broadcast_lobby_join(uint32_t lobby_code, uint8_t player_id,
                               const char *username);
void mesh_broadcast_game_start(const uint8_t *config_data, uint16_t config_len,
                               uint8_t repeat_count);
void mesh_broadcast_player_state(uint8_t player_id, uint8_t team_id,
                                 int32_t lat, int32_t lon,
                                 uint8_t health, uint8_t ammo_pct,
                                 uint8_t alive, uint16_t kills, uint16_t deaths);
void mesh_broadcast_hit_event(uint8_t shooter_id, uint8_t target_id,
                              uint8_t damage, uint8_t remaining_health,
                              uint8_t hit_zone);
void mesh_broadcast_player_death(uint8_t player_id, uint8_t killer_id,
                                 uint16_t final_kills, uint16_t final_deaths,
                                 uint8_t repeat_count);
void mesh_broadcast_game_end(uint8_t reason, uint8_t winning_team,
                             uint8_t repeat_count);
void mesh_broadcast_scoreboard(uint8_t num_teams,
                               const uint8_t *team_ids, const uint16_t *team_kills);
void mesh_broadcast_player_respawn(uint8_t player_id, uint8_t repeat_count);

/* Called from scan callback with received Coded PHY data */
void mesh_process_received(const uint8_t *data, uint16_t len, int8_t rssi);

/* Create the Coded PHY extended advertising set (call once after bt_enable) */
int mesh_create_coded_adv_set(void);

#endif
