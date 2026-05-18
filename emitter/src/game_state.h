#ifndef GAME_STATE_H
#define GAME_STATE_H

#include "protocol.h"
#include <stdbool.h>

void game_state_init(void);
void game_state_set_config(const struct emitter_config *cfg);
const struct emitter_config *game_state_get_config(void);

void game_state_start_game(void);
bool game_state_try_fire(int64_t now_ms);
bool game_state_apply_hit(uint8_t damage);
bool game_state_start_reload(void);
void game_state_complete_reload(void);
void game_state_respawn(void);
void game_state_add_ammo(uint16_t amount);
void game_state_add_kill(void);

uint8_t  game_state_get_health(void);
uint16_t game_state_get_mag_ammo(void);
uint16_t game_state_get_reserve_ammo(void);
bool     game_state_is_alive(void);
bool     game_state_is_reloading(void);
uint16_t game_state_get_kills(void);
uint16_t game_state_get_deaths(void);

#endif
