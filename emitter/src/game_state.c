#include "game_state.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(game_state, LOG_LEVEL_INF);

static struct emitter_config cfg;
static uint8_t  health;
static uint16_t mag_current;
static uint16_t reserve_current;
static bool     alive;
static bool     reloading;
static int64_t  last_shot_ms;
static uint16_t kills;
static uint16_t deaths;

void game_state_init(void)
{
    memset(&cfg, 0, sizeof(cfg));
    health = 0;
    mag_current = 0;
    reserve_current = 0;
    alive = false;
    reloading = false;
    last_shot_ms = 0;
    kills = 0;
    deaths = 0;
}

void game_state_set_config(const struct emitter_config *new_cfg)
{
    cfg = *new_cfg;
    LOG_INF("Config set: dmg=%d mag=%d fire_rate=%dms reload=%dms hp=%d ff=%d",
            cfg.damage, cfg.mag_size, cfg.fire_rate_ms,
            cfg.reload_speed_ms, cfg.max_health, cfg.friendly_fire);
}

const struct emitter_config *game_state_get_config(void)
{
    return &cfg;
}

void game_state_start_game(void)
{
    health = cfg.max_health;
    uint16_t starting_mag = (cfg.initial_total_ammo > cfg.mag_size)
                            ? cfg.mag_size : cfg.initial_total_ammo;
    mag_current = starting_mag;
    reserve_current = cfg.initial_total_ammo - starting_mag;
    alive = true;
    reloading = false;
    last_shot_ms = 0;
    kills = 0;
    deaths = 0;
    LOG_INF("Game started: HP=%d Mag=%d Reserve=%d", health, mag_current, reserve_current);
}

bool game_state_try_fire(int64_t now_ms)
{
    if (!alive || reloading || mag_current == 0) {
        return false;
    }
    if (last_shot_ms > 0 && (now_ms - last_shot_ms) < cfg.fire_rate_ms) {
        return false;
    }
    mag_current--;
    last_shot_ms = now_ms;
    return true;
}

bool game_state_apply_hit(uint8_t damage)
{
    if (!alive) {
        return false;
    }
    if (damage >= health) {
        health = 0;
        alive = false;
        deaths++;
        LOG_INF("DEATH! kills=%d deaths=%d", kills, deaths);
        return true;
    }
    health -= damage;
    LOG_INF("Hit taken: HP=%d", health);
    return false;
}

bool game_state_start_reload(void)
{
    if (reloading) {
        return false;
    }
    if (mag_current == cfg.mag_size) {
        return false;
    }
    if (reserve_current == 0 && mag_current > 0) {
        return false;
    }
    reloading = true;
    LOG_INF("Reloading... (%dms)", cfg.reload_speed_ms);
    return true;
}

void game_state_complete_reload(void)
{
    uint16_t needed = cfg.mag_size - mag_current;
    uint16_t transfer = (reserve_current >= needed) ? needed : reserve_current;
    mag_current += transfer;
    reserve_current -= transfer;
    reloading = false;
    LOG_INF("Reload done: Mag=%d Reserve=%d", mag_current, reserve_current);
}

void game_state_respawn(void)
{
    health = cfg.max_health;
    uint16_t starting_mag = (cfg.initial_total_ammo > cfg.mag_size)
                            ? cfg.mag_size : cfg.initial_total_ammo;
    mag_current = starting_mag;
    reserve_current = cfg.initial_total_ammo - starting_mag;
    alive = true;
    reloading = false;
    LOG_INF("Respawned: HP=%d Mag=%d Reserve=%d", health, mag_current, reserve_current);
}

void game_state_add_ammo(uint16_t amount)
{
    reserve_current += amount;
    LOG_INF("Ammo added: +%d, Reserve=%d", amount, reserve_current);
}

void game_state_add_kill(void)
{
    kills++;
}

uint8_t  game_state_get_health(void)       { return health; }
uint16_t game_state_get_mag_ammo(void)     { return mag_current; }
uint16_t game_state_get_reserve_ammo(void) { return reserve_current; }
bool     game_state_is_alive(void)         { return alive; }
bool     game_state_is_reloading(void)     { return reloading; }
uint16_t game_state_get_kills(void)        { return kills; }
uint16_t game_state_get_deaths(void)       { return deaths; }
