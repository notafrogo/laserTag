#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

#include <string.h>
#include <stdio.h>

#include "protocol.h"
#include "ir_protocol.h"
#include "ble_phone.h"
#include "ble_vest.h"
#include "game_state.h"
#include "mesh.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ===== Hardware ===== */
#define TRIGGER_NODE   DT_ALIAS(trigger_button)
#define PWM_IR_LED_NODE DT_ALIAS(ir_pwm)
#define STATUS_LED_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec trigger = GPIO_DT_SPEC_GET(TRIGGER_NODE, gpios);
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(STATUS_LED_NODE, gpios);
static const struct device *pwm_dev = DEVICE_DT_GET(PWM_IR_LED_NODE);
static struct gpio_callback trigger_cb_data;
#define IR_CARRIER_PERIOD_US 26

/* ===== State ===== */
static enum emitter_state current_state = EMITTER_IDLE;
static struct bt_conn *phone_conn;
static struct player_identity local_player;
static char adv_name[11] = "LT-E-0000";

/* Lobby */
static uint32_t lobby_code;
static uint8_t  lobby_game_mode;
static uint8_t  lobby_host_id;

/* Location */
static int32_t  current_lat;
static int32_t  current_lon;
static uint16_t current_heading;

/* Game config storage */
static uint8_t  game_config_raw[256];
static uint16_t game_config_raw_len;

/* Team kill tracking for scoreboard */
static uint8_t  team_ids[MAX_TEAMS];
static uint16_t team_kills[MAX_TEAMS];
static uint8_t  num_teams;

/* Friendly list */
static uint8_t friendly_ids[MAX_PLAYERS];
static uint8_t friendly_count;

/* Hit dedup ring buffer */
static struct hit_dedup_entry hit_dedup[HIT_DEDUP_SIZE];
static uint8_t hit_dedup_idx;

/* Trigger state */
static bool trigger_ready = true;

/* Pairing — the lowest 2 bytes of our own BLE address. Sent in every
 * IR pairing frame so the vest can match the connecting emitter's BLE
 * address against the IR-supplied value at connected() time. No BLE
 * round-trip / PAIR_CONFIRM write required.
 *
 * Cached once in bt_ready after bt_id_get.
 */
static uint16_t emitter_addr_lsbs;

/* ===== Timers / Work Items ===== */
static struct k_work ir_tx_work;
static struct k_work_delayable pairing_tx_work;
static struct k_work_delayable trigger_work;
static struct k_work_delayable reload_work;
static struct k_work_delayable adv_work;
static struct k_work_delayable lobby_announce_work;
static struct k_work_delayable player_state_work;
static struct k_work_delayable scoreboard_work;
static struct k_timer led_timer;
static int led_tick;

/* Dedicated workqueue for IR TX: ir_tx_send blocks for ~30 ms per shot via
 * k_msleep loops. Running it on the system workqueue stalls every other
 * delayed work (trigger, reload, adv, mesh) for the duration of the burst.
 * Cooperative priority (-1, same as default system workqueue) so kernel
 * preemption doesn't jitter the IR pulse timing — the RX state machine
 * expects 500-2500us marks/spaces and a 10ms preemption tick would corrupt
 * the bit stream. With its own thread, the system workqueue still drains
 * during the k_msleep gaps.
 */
#define IR_TX_STACK_SIZE 1024
#define IR_TX_PRIORITY   (-1)
K_THREAD_STACK_DEFINE(ir_tx_stack, IR_TX_STACK_SIZE);
static struct k_work_q ir_tx_q;

/* Config persistence */
static struct emitter_config saved_config;
static atomic_t config_loaded = ATOMIC_INIT(0);

/* ===== Forward Declarations ===== */
static void send_state_update(void);
static void start_advertising(void);

/* ===== LED Timer ===== */
static void led_timer_handler(struct k_timer *timer_id)
{
    led_tick++;
    if (!phone_conn) {
        gpio_pin_set_dt(&status_led, (led_tick % 10 == 0) ? 1 : 0);
    }
}

/* ===== Notifications to Phone ===== */

static void phone_notify(const uint8_t *data, uint16_t len)
{
    ble_phone_notify(phone_conn, data, len);
}

static void phone_ack(uint8_t opcode)
{
    phone_notify(&opcode, 1);
}

static void send_state_update(void)
{
    uint8_t buf[6];
    buf[0] = RSP_STATE_UPDATE;
    buf[1] = game_state_get_health();
    sys_put_le16(game_state_get_mag_ammo(), &buf[2]);
    sys_put_le16(game_state_get_reserve_ammo(), &buf[4]);
    phone_notify(buf, 6);
}

static void send_death_notify(uint8_t killer_id)
{
    uint8_t buf[2] = { RSP_DEATH_NOTIFY, killer_id };
    phone_notify(buf, 2);
}

/* ===== Config Parsing ===== */

static void parse_emitter_config_from_bytes(const uint8_t *data, struct emitter_config *cfg)
{
    cfg->user_id           = data[0];
    cfg->damage            = data[1];
    cfg->mag_size          = sys_get_le16(&data[2]);
    cfg->fire_rate_ms      = sys_get_le16(&data[4]);
    cfg->reload_speed_ms   = sys_get_le16(&data[6]);
    cfg->full_auto         = data[8];
    cfg->ammo_type         = data[9];
    cfg->initial_total_ammo = sys_get_le16(&data[10]);
    cfg->max_health        = data[12];
    cfg->friendly_fire     = data[13];
}

static void apply_game_config(const uint8_t *data, uint16_t len)
{
    if (len < 10) {
        return;
    }

    uint16_t pos = 0;
    /* uint8_t game_mode = data[pos]; */ pos += 1;
    /* uint16_t time_limit = sys_get_le16(&data[pos]); */ pos += 2;
    /* uint16_t score_limit = sys_get_le16(&data[pos]); */ pos += 2;
    /* uint16_t loc_interval = sys_get_le16(&data[pos]); */ pos += 2;
    /* uint16_t respawn_time = sys_get_le16(&data[pos]); */ pos += 2;
    uint8_t n_teams   = data[pos++];
    uint8_t n_players = data[pos++];

    num_teams = (n_teams > MAX_TEAMS) ? MAX_TEAMS : n_teams;
    for (int i = 0; i < n_teams; i++) {
        if (pos + 2 > len) {
            return;
        }
        if (i < MAX_TEAMS) {
            team_ids[i] = data[pos];
            team_kills[i] = 0;
        }
        pos++;
        uint8_t name_len = data[pos++];
        if (pos + name_len > len) {
            return;
        }
        pos += name_len;
    }

    /* Build friendly list: players on same team */
    friendly_count = 0;
    uint16_t player_section = pos;
    for (int i = 0; i < n_players; i++) {
        if (player_section + 3 > len) {
            return;
        }
        uint8_t pid = data[player_section++];
        uint8_t tid = data[player_section++];
        uint8_t ulen = data[player_section++];
        if (player_section + ulen > len) {
            return;
        }
        player_section += ulen;

        if (tid == local_player.team_id && pid != local_player.player_id) {
            if (friendly_count < MAX_PLAYERS) {
                friendly_ids[friendly_count++] = pid;
            }
        }
    }
    pos = player_section;

    /* Emitter config (14 bytes) */
    if (pos + EMITTER_CONFIG_SIZE <= len) {
        struct emitter_config cfg;
        parse_emitter_config_from_bytes(&data[pos], &cfg);
        cfg.user_id = local_player.player_id;
        game_state_set_config(&cfg);
    }
}

/* ===== Vest Hit Callback ===== */

static bool hit_dedup_check(uint16_t packet_id)
{
    for (int i = 0; i < HIT_DEDUP_SIZE; i++) {
        if (hit_dedup[i].used && hit_dedup[i].packet_id == packet_id) {
            return true;
        }
    }
    hit_dedup[hit_dedup_idx].packet_id = packet_id;
    hit_dedup[hit_dedup_idx].used = true;
    hit_dedup_idx = (hit_dedup_idx + 1) % HIT_DEDUP_SIZE;
    return false;
}

static void on_vest_hit(uint16_t packet_id, uint8_t shooter_id,
                        uint8_t weapon_id, uint8_t team_id)
{
    if (current_state != EMITTER_GAME_ACTIVE && current_state != EMITTER_DEAD) {
        return;
    }

    ble_vest_send_hit_ack(packet_id);

    if (hit_dedup_check(packet_id)) {
        return;
    }

    if (current_state != EMITTER_GAME_ACTIVE) {
        return;
    }

    const struct emitter_config *cfg = game_state_get_config();
    uint8_t damage = cfg->damage;

    bool died = game_state_apply_hit(damage);
    send_state_update();

    mesh_broadcast_hit_event(shooter_id, local_player.player_id,
                             damage, game_state_get_health(), 0);

    if (died) {
        current_state = EMITTER_DEAD;
        ble_vest_send_death();
        send_death_notify(shooter_id);
        mesh_broadcast_player_death(local_player.player_id, shooter_id,
                                    game_state_get_kills(),
                                    game_state_get_deaths(), 3);
        k_work_cancel_delayable(&player_state_work);
        k_work_cancel_delayable(&scoreboard_work);
    }
}

static void on_vest_connected(const uint8_t *mac)
{
    /* BLE link is up but GATT discovery isn't done yet — defer the
     * RSP_VEST_PAIRED notification to on_vest_gatt_ready so the phone
     * doesn't see "paired" before commands actually flow. The mac is
     * kept in ble_vest's state and pulled later via ble_vest_get_active_mac.
     */
    (void)mac;
    LOG_INF("Vest BLE connected, awaiting GATT");
}

static void on_vest_disconnected(uint8_t reason)
{
    PLOG_WRN("Vest disconnected (reason %u)", reason);
    if (phone_conn) {
        uint8_t buf[2] = { RSP_VEST_DISCONNECTED, reason };
        phone_notify(buf, 2);
    }
}

/* ===== IR TX Work ===== */

static void ir_tx_work_handler(struct k_work *work)
{
    ir_packet_t pkt = {
        .player_id = local_player.player_id,
        .weapon_id = 0x00,
        .team_id   = local_player.team_id,
    };
    ir_tx_send(&pkt);
}

/* Fires one pairing IR frame, then reschedules itself while the trigger
 * is still held in EMITTER_PAIRING. Runs on the cooperative IR workqueue
 * so the ~120 ms blocking TX doesn't stall the system workqueue and
 * keeps deterministic pulse timing.
 */
static void pairing_tx_work_handler(struct k_work *work)
{
    if (current_state != EMITTER_PAIRING) {
        return;
    }
    if (gpio_pin_get_dt(&trigger) != 1) {
        return;
    }
    /* Once the BLE link to the vest is up, stop firing. The vest has
     * already extracted the pairing identity (our MAC LSBs) from the IR
     * frame it received before connecting; continuing to blast IR from
     * this cooperative workqueue just contends with the system workqueue
     * and BT host. The user is free to keep holding the trigger — we
     * just don't need IR anymore once we have a link.
     */
    if (ble_vest_link_up()) {
        return;
    }

    ir_pairing_packet_t pkt = {
        .addr_lsbs = emitter_addr_lsbs,
        .player_id = local_player.player_id,
    };
    ir_tx_send_pairing(&pkt);

    k_work_reschedule_for_queue(&ir_tx_q, &pairing_tx_work, K_NO_WAIT);
}

/* Called by ble_vest once GATT discovery + subscription complete and
 * commands can flow. Pairing identity is verified by the vest at
 * connected() time using the BLE address we embed in the IR pairing
 * frame, so there's no PAIR_CONFIRM write here. We just notify the
 * phone that the vest is paired and ready — doing it after GATT-ready
 * (not on raw BLE-connect) means the phone sees "paired" only once
 * commands can actually flow.
 */
static void on_vest_gatt_ready(void)
{
    uint8_t vmac[6];
    if (ble_vest_get_active_mac(vmac)) {
        uint8_t buf[7];
        buf[0] = RSP_VEST_PAIRED;
        memcpy(&buf[1], vmac, 6);
        phone_notify(buf, 7);
        PLOG_INF("Vest paired, notified phone");
    } else {
        PLOG_WRN("GATT ready but no active vest MAC — not sending RSP_VEST_PAIRED");
    }
}

/* ===== Trigger Handling ===== */

static void trigger_work_handler(struct k_work *work)
{
    bool pressed = (gpio_pin_get_dt(&trigger) == 1);

    if (!pressed) {
        trigger_ready = true;
        return;
    }

    if (current_state == EMITTER_PAIRING) {
        /* Fire IR pairing frames while the trigger is held. The handler
         * reschedules itself until the trigger releases or we leave
         * EMITTER_PAIRING. Submitting again while already pending is a
         * no-op, so repeated trigger-press IRQs are harmless.
         */
        k_work_reschedule_for_queue(&ir_tx_q, &pairing_tx_work, K_NO_WAIT);
        return;
    }

    if (current_state != EMITTER_GAME_ACTIVE) {
        return;
    }

    if (game_state_is_reloading()) {
        return;
    }

    if (game_state_get_mag_ammo() == 0) {
        return;
    }

    const struct emitter_config *cfg = game_state_get_config();
    if (!cfg->full_auto && !trigger_ready) {
        return;
    }

    int64_t now = k_uptime_get();
    if (!game_state_try_fire(now)) {
        k_work_reschedule(&trigger_work, K_MSEC(cfg->fire_rate_ms));
        return;
    }

    trigger_ready = false;
    send_state_update();
    k_work_submit_to_queue(&ir_tx_q, &ir_tx_work);

    if (cfg->full_auto && game_state_get_mag_ammo() > 0) {
        k_work_reschedule(&trigger_work, K_MSEC(cfg->fire_rate_ms));
    }
}

static void trigger_pressed_cb(const struct device *dev, struct gpio_callback *cb,
                                uint32_t pins)
{
    k_work_reschedule(&trigger_work, K_NO_WAIT);
}

/* ===== Reload ===== */

static void reload_work_handler(struct k_work *work)
{
    game_state_complete_reload();
    send_state_update();
}

/* ===== Periodic Broadcasts ===== */

static void lobby_announce_handler(struct k_work *work)
{
    if (current_state != EMITTER_LOBBY_HOST) {
        return;
    }
    mesh_broadcast_lobby_announce(lobby_code, lobby_game_mode,
                                  local_player.username, 1);
    k_work_reschedule(&lobby_announce_work, K_MSEC(500));
}

static void player_state_handler(struct k_work *work)
{
    if (current_state != EMITTER_GAME_ACTIVE) {
        return;
    }

    const struct emitter_config *cfg = game_state_get_config();
    uint16_t mag = game_state_get_mag_ammo();
    uint16_t res = game_state_get_reserve_ammo();
    uint16_t total_max = cfg->initial_total_ammo;
    uint8_t ammo_pct = total_max > 0 ? (uint8_t)((((uint32_t)mag + res) * 100) / total_max) : 0;

    mesh_broadcast_player_state(local_player.player_id, local_player.team_id,
                                current_lat, current_lon,
                                game_state_get_health(), ammo_pct,
                                game_state_is_alive() ? 1 : 0,
                                game_state_get_kills(),
                                game_state_get_deaths());

    k_work_reschedule(&player_state_work, K_MSEC(3000));
}

static void scoreboard_handler(struct k_work *work)
{
    if (current_state != EMITTER_GAME_ACTIVE && current_state != EMITTER_DEAD) {
        return;
    }
    mesh_broadcast_scoreboard(num_teams, team_ids, team_kills);
    k_work_reschedule(&scoreboard_work, K_MSEC(10000));
}

/* ===== Phone Command Handler ===== */

static void on_phone_rx(const uint8_t *data, uint16_t len)
{
    if (len == 0) {
        return;
    }

    switch (data[0]) {

    case CMD_CONFIG:
        if (len >= 1 + EMITTER_CONFIG_SIZE) {
            struct emitter_config cfg;
            parse_emitter_config_from_bytes(&data[1], &cfg);
            game_state_set_config(&cfg);
            settings_save_one("emitter/config", &cfg, sizeof(cfg));
            phone_ack(RSP_CONFIG_ACK);
        }
        break;

    case CMD_START:
        game_state_start_game();
        current_state = EMITTER_GAME_ACTIVE;

        if (ble_vest_is_connected()) {
            ble_vest_send_friendly_list(friendly_ids, friendly_count);
            const struct emitter_config *cfg = game_state_get_config();
            ble_vest_send_friendly_fire(cfg->friendly_fire);
        }

        phone_ack(RSP_START_ACK);
        send_state_update();
        k_work_reschedule(&player_state_work, K_MSEC(3000));
        k_work_reschedule(&scoreboard_work, K_MSEC(10000));
        break;

    case CMD_AMMO_INCREASE:
        if (current_state == EMITTER_GAME_ACTIVE && len >= 3) {
            uint16_t amount = sys_get_le16(&data[1]);
            game_state_add_ammo(amount);
            phone_ack(RSP_AMMO_ACK);
            send_state_update();
        }
        break;

    case CMD_GAME_OVER:
        k_work_cancel_delayable(&player_state_work);
        k_work_cancel_delayable(&scoreboard_work);
        k_work_cancel_delayable(&reload_work);
        k_work_cancel_delayable(&trigger_work);
        current_state = EMITTER_IDLE;
        phone_ack(RSP_GAME_OVER_ACK);
        break;

    case CMD_BROADCAST_CODED_PHY:
        if (len > 1) {
            mesh_broadcast_raw(&data[1], len - 1);
            phone_ack(RSP_BROADCAST_ACK);
        }
        break;

    case CMD_SET_PLAYER_INFO:
        if (len >= 3) {
            local_player.player_id = data[1];
            local_player.team_id = data[2];
            uint8_t name_len = len - 3;
            if (name_len > MAX_USERNAME_LEN) name_len = MAX_USERNAME_LEN;
            memcpy(local_player.username, &data[3], name_len);
            local_player.username[name_len] = '\0';
            mesh_set_identity(local_player.player_id, local_player.team_id);
            LOG_INF("Player: id=%d team=%d name=%s",
                    local_player.player_id, local_player.team_id, local_player.username);
            phone_ack(RSP_PLAYER_INFO_ACK);
        }
        break;

    case CMD_START_LOBBY_HOST:
        if (len >= 7) {
            lobby_code = sys_get_le32(&data[1]);
            lobby_game_mode = data[5];
            lobby_host_id = data[6];
            current_state = EMITTER_LOBBY_HOST;
            k_work_reschedule(&lobby_announce_work, K_NO_WAIT);
            phone_ack(RSP_LOBBY_HOST_ACK);
        }
        break;

    case CMD_START_LOBBY_SCAN:
        current_state = EMITTER_LOBBY_SCAN;
        phone_ack(RSP_LOBBY_SCAN_ACK);
        break;

    case CMD_STOP_LOBBY:
        k_work_cancel_delayable(&lobby_announce_work);
        current_state = EMITTER_IDLE;
        break;

    case CMD_BROADCAST_GAME_CFG:
        if (len > 1) {
            game_config_raw_len = len - 1;
            memcpy(game_config_raw, &data[1], game_config_raw_len);

            apply_game_config(game_config_raw, game_config_raw_len);
            game_state_start_game();
            current_state = EMITTER_GAME_ACTIVE;

            mesh_broadcast_game_start(game_config_raw, game_config_raw_len, 5);

            if (ble_vest_is_connected()) {
                ble_vest_send_friendly_list(friendly_ids, friendly_count);
                const struct emitter_config *cfg = game_state_get_config();
                ble_vest_send_friendly_fire(cfg->friendly_fire);
            }

            phone_ack(RSP_GAME_CFG_ACK);
            send_state_update();
            k_work_reschedule(&player_state_work, K_MSEC(3000));
            k_work_reschedule(&scoreboard_work, K_MSEC(10000));
        }
        break;

    case CMD_UPDATE_LOCATION:
        if (len >= 11) {
            current_lat = (int32_t)sys_get_le32(&data[1]);
            current_lon = (int32_t)sys_get_le32(&data[5]);
            current_heading = sys_get_le16(&data[9]);
            phone_ack(RSP_LOCATION_ACK);
        }
        break;

    case CMD_REQUEST_MESH_STATS:
        phone_ack(RSP_MESH_STATS_ACK);
        break;

    case CMD_ENTER_PAIRING:
        current_state = EMITTER_PAIRING;
        phone_ack(RSP_PAIRING_ACK);
        PLOG_INF("Pairing mode: shoot the vest (addr_lsbs=0x%04x)", emitter_addr_lsbs);
        break;

    case CMD_UNPAIR_VEST:
        /* Tell the vest to clear its NVS first so it doesn't auto-reconnect
         * after we drop the link.
         */
        if (ble_vest_is_connected()) {
            ble_vest_send_unpair();
        }
        ble_vest_disconnect();
        break;

    case CMD_RESPAWN:
        if (current_state == EMITTER_DEAD) {
            game_state_respawn();
            current_state = EMITTER_GAME_ACTIVE;

            if (ble_vest_is_connected()) {
                ble_vest_send_respawn();
            }

            mesh_broadcast_player_respawn(local_player.player_id, 3);
            phone_ack(RSP_RESPAWN_ACK);
            send_state_update();
            k_work_reschedule(&player_state_work, K_MSEC(3000));
            k_work_reschedule(&scoreboard_work, K_MSEC(10000));
        }
        break;

    case CMD_RELOAD:
        if (current_state == EMITTER_GAME_ACTIVE) {
            const struct emitter_config *cfg = game_state_get_config();
            if (game_state_start_reload()) {
                k_work_reschedule(&reload_work, K_MSEC(cfg->reload_speed_ms));
            }
        }
        break;
    }
}

/* ===== Coded PHY Scan Callback ===== */

static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
    if (buf->len == 0) {
        return;
    }

    /* Diagnostic: log every 32nd adv (rate-limited). primary_phy is 0x01
     * for 1M, 0x03 for Coded — useful for bring-up.
     */
    static uint32_t adv_log_count;
    if ((adv_log_count++ & 0x1F) == 0) {
        char addr_str[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(info->addr, addr_str, sizeof(addr_str));
        LOG_INF("scan_recv: addr=%s rssi=%d phy=%u len=%u",
                addr_str, info->rssi, info->primary_phy, buf->len);
    }

    /* Vest discovery on 1M PHY (during pairing or reconnection) */
    if (current_state == EMITTER_PAIRING || !ble_vest_is_connected()) {
        ble_vest_on_scan_result(info->addr, info->rssi, buf);
    }

    /* Mesh ingest is restricted to Coded PHY — our mesh broadcasts go out
     * on Coded only, and the 1M band is dominated by unrelated iBeacons
     * and OS-level adverts. If we let 1M traffic through, every nearby
     * Apple device's adv gets misparsed as a mesh packet, dedup-cached,
     * and forwarded to the phone over GATT — saturating the BT host's
     * notify queue and blocking downstream GATT operations.
     *
     * Also: buf->data is the full BLE AD-record blob, not the raw mesh
     * payload. We need to walk records and pull out the
     * BT_DATA_MANUFACTURER_DATA contents (where mesh.c puts our header
     * via BT_DATA(BT_DATA_MANUFACTURER_DATA, ...)).
     */
    if (phone_conn && info->primary_phy == BT_GAP_LE_PHY_CODED) {
        const uint8_t *d = buf->data;
        uint16_t pos = 0;
        while (pos + 1 < buf->len) {
            uint8_t ad_len = d[pos];
            if (ad_len == 0 || pos + 1 + ad_len > buf->len) {
                break;
            }
            uint8_t ad_type = d[pos + 1];
            if (ad_type == BT_DATA_MANUFACTURER_DATA && ad_len >= 1) {
                uint16_t payload_len = ad_len - 1;
                mesh_process_received(&d[pos + 2], payload_len, info->rssi);
                break;
            }
            pos += ad_len + 1;
        }
    }
}

static struct bt_le_scan_cb scan_callbacks = {
    .recv = scan_recv,
};

/* ===== BLE Connection Callbacks ===== */

static void adv_work_handler(struct k_work *work)
{
    start_advertising();
}

static void start_advertising(void)
{
    struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, strlen(adv_name)),
    };
    struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
        BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_SCANNABLE,
        BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

    int err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err && err != -EALREADY) {
        LOG_ERR("Adv start failed (err %d)", err);
    }
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed (err 0x%02x)", err);
        return;
    }

    struct bt_conn_info info;
    bt_conn_get_info(conn, &info);

    /* Only handle incoming connections (phone → emitter peripheral) */
    if (info.role != BT_CONN_ROLE_PERIPHERAL) {
        return;
    }

    const bt_addr_le_t *addr = bt_conn_get_dst(conn);
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    LOG_INF("Phone connected: %s", addr_str);

    phone_conn = bt_conn_ref(conn);
    mesh_set_phone_conn(phone_conn);
    ble_phone_log_set_conn(phone_conn);
    gpio_pin_set_dt(&status_led, 1);

    /* If a vest is already paired to us, the phone never saw the original
     * RSP_VEST_PAIRED notification — it was dropped because phone_conn was
     * NULL at the time. Re-send it now so the app can render vest state.
     */
    uint8_t vmac[6];
    if (ble_vest_get_active_mac(vmac)) {
        uint8_t buf[7];
        buf[0] = RSP_VEST_PAIRED;
        memcpy(&buf[1], vmac, 6);
        phone_notify(buf, 7);
        PLOG_INF("Re-notified phone of already-paired vest");
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    if (conn == phone_conn) {
        LOG_INF("Phone disconnected (reason %d)", reason);
        bt_conn_unref(phone_conn);
        phone_conn = NULL;
        mesh_set_phone_conn(NULL);
        ble_phone_log_set_conn(NULL);
        gpio_pin_set_dt(&status_led, 0);
        k_work_reschedule(&adv_work, K_MSEC(100));
    }
}

BT_CONN_CB_DEFINE(phone_conn_cbs) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* ===== BT Ready ===== */

static void bt_ready(int err)
{
    if (err) {
        LOG_ERR("BT init failed (err %d)", err);
        return;
    }

    /* Set device name: LT-E-XXXX and cache the lowest 2 address bytes
     * for inclusion in IR pairing frames. */
    bt_addr_le_t addrs[1];
    size_t count = 1;
    bt_id_get(addrs, &count);
    if (count > 0) {
        snprintf(adv_name, sizeof(adv_name), "LT-E-%02X%02X",
                 addrs[0].a.val[1], addrs[0].a.val[0]);
        emitter_addr_lsbs = (uint16_t)addrs[0].a.val[0] |
                            ((uint16_t)addrs[0].a.val[1] << 8);
    }
    bt_set_name(adv_name);
    LOG_INF("Device name: %s (addr_lsbs=0x%04x)", adv_name, emitter_addr_lsbs);

    start_advertising();

    /* Coded PHY mesh setup */
    mesh_create_coded_adv_set();
    bt_le_scan_cb_register(&scan_callbacks);
    mesh_start_scanner();
}

/* ===== Settings (NVS persistence) ===== */

static int settings_set(const char *name, size_t len,
                        settings_read_cb read_cb, void *cb_arg)
{
    if (strcmp(name, "config") == 0 && len == sizeof(saved_config)) {
        read_cb(cb_arg, &saved_config, sizeof(saved_config));
        game_state_set_config(&saved_config);
        atomic_set(&config_loaded, 1);
        return 0;
    }
    return -ENOENT;
}

static struct settings_handler settings_h = {
    .name = "emitter",
    .h_set = settings_set,
};

/* ===== Main ===== */

int main(void)
{
    /* GPIO setup */
    gpio_pin_configure_dt(&trigger, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&trigger, GPIO_INT_EDGE_BOTH);
    gpio_init_callback(&trigger_cb_data, trigger_pressed_cb, BIT(trigger.pin));
    gpio_add_callback(trigger.port, &trigger_cb_data);
    gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);

    /* Dedicated IR TX workqueue (ir_tx_send blocks ~30 ms per shot) */
    k_work_queue_init(&ir_tx_q);
    k_work_queue_start(&ir_tx_q, ir_tx_stack, K_THREAD_STACK_SIZEOF(ir_tx_stack),
                       IR_TX_PRIORITY, NULL);

    /* Work items */
    k_work_init(&ir_tx_work, ir_tx_work_handler);
    k_work_init_delayable(&pairing_tx_work, pairing_tx_work_handler);
    k_work_init_delayable(&trigger_work, trigger_work_handler);
    k_work_init_delayable(&reload_work, reload_work_handler);
    k_work_init_delayable(&adv_work, adv_work_handler);
    k_work_init_delayable(&lobby_announce_work, lobby_announce_handler);
    k_work_init_delayable(&player_state_work, player_state_handler);
    k_work_init_delayable(&scoreboard_work, scoreboard_handler);

    /* LED timer */
    k_timer_init(&led_timer, led_timer_handler, NULL);
    k_timer_start(&led_timer, K_MSEC(100), K_MSEC(100));

    /* IR transmitter */
    ir_tx_init(pwm_dev, IR_CARRIER_PERIOD_US);

    /* Game state */
    game_state_init();

    /* Mesh */
    mesh_init();

    /* Vest BLE central */
    ble_vest_init(on_vest_hit, on_vest_connected, on_vest_disconnected,
                  on_vest_gatt_ready);

    /* Settings */
    settings_subsys_init();
    settings_register(&settings_h);
    settings_load();

    /* Phone BLE peripheral */
    ble_phone_init(on_phone_rx);
    bt_enable(bt_ready);

    return 0;
}
