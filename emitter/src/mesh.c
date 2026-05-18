#include "mesh.h"
#include "ble_phone.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(mesh, LOG_LEVEL_INF);

/* ===== State ===== */
static uint8_t local_player_id;
static uint8_t local_team_id;
static struct bt_conn *phone_conn;
static uint16_t seq_counter;

/* ===== Coded PHY Advertising Set ===== */
static struct bt_le_ext_adv *coded_adv_set;
static struct k_work_delayable stop_adv_work;
static atomic_t is_broadcasting = ATOMIC_INIT(0);

/* ===== Dedup Ring Buffer ===== */
struct dedup_entry {
    uint8_t  origin_id;
    uint16_t seq_num;
    bool     used;
};
static struct dedup_entry dedup_buf[MESH_DEDUP_SIZE];
static uint8_t dedup_idx;

/* ===== Scan ===== */
static struct bt_le_scan_param coded_scan_param = {
    .type     = BT_LE_SCAN_TYPE_PASSIVE,
    .options  = BT_LE_SCAN_OPT_CODED,
    .interval = 0x00A0,
    .window   = 0x00A0,
};

/* ===== Repeat broadcast state ===== */
static uint8_t repeat_buf[256];
static uint16_t repeat_len;
static uint8_t repeat_remaining;
static struct k_work_delayable repeat_work;

/* ===== Relay work (avoids blocking BT thread) ===== */
static uint8_t relay_buf_pending[256];
static uint16_t relay_len_pending;
static struct k_work_delayable relay_work;

/* ===== Internal ===== */

static bool dedup_check_and_insert(uint8_t origin_id, uint16_t seq_num)
{
    for (int i = 0; i < MESH_DEDUP_SIZE; i++) {
        if (dedup_buf[i].used &&
            dedup_buf[i].origin_id == origin_id &&
            dedup_buf[i].seq_num == seq_num) {
            return true;
        }
    }
    dedup_buf[dedup_idx].origin_id = origin_id;
    dedup_buf[dedup_idx].seq_num = seq_num;
    dedup_buf[dedup_idx].used = true;
    dedup_idx = (dedup_idx + 1) % MESH_DEDUP_SIZE;
    return false;
}

static void do_coded_broadcast(const uint8_t *data, uint16_t len)
{
    if (!coded_adv_set || len == 0) {
        return;
    }

    bt_le_scan_stop();

    struct bt_data ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA, data, len)
    };

    int err = bt_le_ext_adv_set_data(coded_adv_set, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("Coded PHY set data failed (err %d)", err);
        mesh_start_scanner();
        return;
    }

    struct bt_le_ext_adv_start_param start_param = {
        .timeout = 0,
        .num_events = 1,
    };
    err = bt_le_ext_adv_start(coded_adv_set, &start_param);
    if (err) {
        LOG_ERR("Coded PHY adv start failed (err %d)", err);
        mesh_start_scanner();
        return;
    }

    atomic_set(&is_broadcasting, 1);
    k_work_reschedule(&stop_adv_work, K_MSEC(500));
}

static void stop_adv_handler(struct k_work *work)
{
    bt_le_ext_adv_stop(coded_adv_set);
    atomic_set(&is_broadcasting, 0);
    mesh_start_scanner();
}

static void repeat_work_handler(struct k_work *work)
{
    if (repeat_remaining > 0) {
        repeat_remaining--;
        do_coded_broadcast(repeat_buf, repeat_len);
        if (repeat_remaining > 0) {
            k_work_reschedule(&repeat_work, K_MSEC(200));
        }
    }
}

static uint16_t next_seq(void)
{
    return seq_counter++;
}

static uint16_t build_mesh_packet(uint8_t *buf, size_t buf_size,
                                  uint8_t mesh_type, const uint8_t *payload,
                                  uint8_t payload_len)
{
    if (MESH_HEADER_SIZE + payload_len > buf_size) {
        return 0;
    }
    struct mesh_header *hdr = (struct mesh_header *)buf;
    hdr->mesh_type = mesh_type;
    hdr->origin_id = local_player_id;
    hdr->seq_num = sys_cpu_to_le16(next_seq());
    hdr->ttl = MESH_DEFAULT_TTL;
    hdr->payload_len = payload_len;
    if (payload_len > 0 && payload) {
        memcpy(buf + MESH_HEADER_SIZE, payload, payload_len);
    }
    return MESH_HEADER_SIZE + payload_len;
}

static void broadcast_with_repeat(uint8_t mesh_type, const uint8_t *payload,
                                  uint8_t payload_len, uint8_t repeat_count)
{
    uint8_t buf[256];
    uint16_t len = build_mesh_packet(buf, sizeof(buf), mesh_type, payload, payload_len);
    if (len == 0) {
        return;
    }
    do_coded_broadcast(buf, len);

    if (repeat_count > 1) {
        memcpy(repeat_buf, buf, len);
        repeat_len = len;
        repeat_remaining = repeat_count - 1;
        k_work_reschedule(&repeat_work, K_MSEC(200));
    }
}

static void forward_to_phone(const uint8_t *data, uint16_t len)
{
    if (!phone_conn || len == 0 || len > 254) {
        return;
    }
    uint8_t notify_buf[255];
    notify_buf[0] = 0x05;
    memcpy(&notify_buf[1], data, len);
    ble_phone_notify(phone_conn, notify_buf, len + 1);
}

static void relay_work_handler(struct k_work *work)
{
    do_coded_broadcast(relay_buf_pending, relay_len_pending);
}

static void relay_message(const uint8_t *data, uint16_t len)
{
    if (len < MESH_HEADER_SIZE || len > sizeof(relay_buf_pending)) {
        return;
    }

    memcpy(relay_buf_pending, data, len);
    relay_len_pending = len;

    struct mesh_header *hdr = (struct mesh_header *)relay_buf_pending;
    hdr->ttl = hdr->ttl - 1;

    uint32_t jitter = MESH_RELAY_JITTER_MIN +
                      (sys_rand32_get() % (MESH_RELAY_JITTER_MAX - MESH_RELAY_JITTER_MIN + 1));
    k_work_reschedule(&relay_work, K_MSEC(jitter));
}

/* ===== Public API ===== */

void mesh_init(void)
{
    k_work_init_delayable(&stop_adv_work, stop_adv_handler);
    k_work_init_delayable(&repeat_work, repeat_work_handler);
    k_work_init_delayable(&relay_work, relay_work_handler);
    memset(dedup_buf, 0, sizeof(dedup_buf));
    dedup_idx = 0;
    seq_counter = 0;
}

void mesh_set_identity(uint8_t player_id, uint8_t team_id)
{
    local_player_id = player_id;
    local_team_id = team_id;
}

void mesh_set_phone_conn(struct bt_conn *conn)
{
    phone_conn = conn;
}

void mesh_start_scanner(void)
{
    int err = bt_le_scan_start(&coded_scan_param, NULL);
    if (err && err != -EALREADY) {
        LOG_ERR("Coded PHY scanner start failed (err %d)", err);
    }
}

void mesh_stop_scanner(void)
{
    bt_le_scan_stop();
}

void mesh_broadcast_raw(const uint8_t *data, uint16_t len)
{
    do_coded_broadcast(data, len);
}

void mesh_broadcast_lobby_announce(uint32_t lobby_code, uint8_t game_mode,
                                   const char *host_name, uint8_t player_count)
{
    uint8_t payload[32];
    uint8_t idx = 0;
    sys_put_le32(lobby_code, &payload[idx]); idx += 4;
    payload[idx++] = game_mode;
    uint8_t name_len = strlen(host_name);
    if (name_len > 16) name_len = 16;
    memcpy(&payload[idx], host_name, name_len); idx += name_len;
    payload[idx++] = player_count;

    uint8_t buf[256];
    uint16_t len = build_mesh_packet(buf, sizeof(buf), MESH_LOBBY_ANNOUNCE, payload, idx);
    if (len > 0) {
        do_coded_broadcast(buf, len);
    }
}

void mesh_broadcast_lobby_join(uint32_t lobby_code, uint8_t player_id,
                               const char *username)
{
    uint8_t payload[32];
    uint8_t idx = 0;
    sys_put_le32(lobby_code, &payload[idx]); idx += 4;
    payload[idx++] = player_id;
    uint8_t name_len = strlen(username);
    if (name_len > 16) name_len = 16;
    memcpy(&payload[idx], username, name_len); idx += name_len;
    broadcast_with_repeat(MESH_LOBBY_JOIN, payload, idx, 3);
}

void mesh_broadcast_game_start(const uint8_t *config_data, uint16_t config_len,
                               uint8_t repeat_count)
{
    broadcast_with_repeat(MESH_GAME_START, config_data, config_len, repeat_count);
}

void mesh_broadcast_player_state(uint8_t player_id, uint8_t team_id,
                                 int32_t lat, int32_t lon,
                                 uint8_t health, uint8_t ammo_pct,
                                 uint8_t alive, uint16_t kills, uint16_t deaths)
{
    uint8_t payload[17];
    payload[0] = player_id;
    payload[1] = team_id;
    sys_put_le32((uint32_t)lat, &payload[2]);
    sys_put_le32((uint32_t)lon, &payload[6]);
    payload[10] = health;
    payload[11] = ammo_pct;
    payload[12] = alive;
    sys_put_le16(kills, &payload[13]);
    sys_put_le16(deaths, &payload[15]);

    uint8_t buf[256];
    uint16_t len = build_mesh_packet(buf, sizeof(buf), MESH_PLAYER_STATE, payload, 17);
    if (len > 0) {
        do_coded_broadcast(buf, len);
    }
}

void mesh_broadcast_hit_event(uint8_t shooter_id, uint8_t target_id,
                              uint8_t damage, uint8_t remaining_health,
                              uint8_t hit_zone)
{
    uint8_t payload[5] = { shooter_id, target_id, damage, remaining_health, hit_zone };
    uint8_t buf[256];
    uint16_t len = build_mesh_packet(buf, sizeof(buf), MESH_HIT_EVENT, payload, 5);
    if (len > 0) {
        do_coded_broadcast(buf, len);
    }
}

void mesh_broadcast_player_death(uint8_t player_id, uint8_t killer_id,
                                 uint16_t final_kills, uint16_t final_deaths,
                                 uint8_t repeat_count)
{
    uint8_t payload[6];
    payload[0] = player_id;
    payload[1] = killer_id;
    sys_put_le16(final_kills, &payload[2]);
    sys_put_le16(final_deaths, &payload[4]);
    broadcast_with_repeat(MESH_PLAYER_DEATH, payload, 6, repeat_count);
}

void mesh_broadcast_game_end(uint8_t reason, uint8_t winning_team,
                             uint8_t repeat_count)
{
    uint8_t payload[2] = { reason, winning_team };
    broadcast_with_repeat(MESH_GAME_END, payload, 2, repeat_count);
}

void mesh_broadcast_scoreboard(uint8_t num_teams,
                               const uint8_t *team_ids, const uint16_t *team_kills)
{
    uint8_t payload[32];
    uint8_t idx = 0;
    payload[idx++] = num_teams;
    for (int i = 0; i < num_teams && idx + 3 <= sizeof(payload); i++) {
        payload[idx++] = team_ids[i];
        sys_put_le16(team_kills[i], &payload[idx]); idx += 2;
    }

    uint8_t buf[256];
    uint16_t len = build_mesh_packet(buf, sizeof(buf), MESH_SCOREBOARD, payload, idx);
    if (len > 0) {
        do_coded_broadcast(buf, len);
    }
}

void mesh_broadcast_player_respawn(uint8_t player_id, uint8_t repeat_count)
{
    uint8_t payload[1] = { player_id };
    broadcast_with_repeat(MESH_PLAYER_RESPAWN, payload, 1, repeat_count);
}

void mesh_process_received(const uint8_t *data, uint16_t len, int8_t rssi)
{
    if (len < MESH_HEADER_SIZE) {
        return;
    }

    struct mesh_header hdr;
    hdr.mesh_type   = data[0];
    hdr.origin_id   = data[1];
    hdr.seq_num     = sys_get_le16(&data[2]);
    hdr.ttl         = data[4];
    hdr.payload_len = data[5];

    if (hdr.origin_id == local_player_id) {
        return;
    }

    if (dedup_check_and_insert(hdr.origin_id, hdr.seq_num)) {
        return;
    }

    LOG_INF("Mesh RX: type=0x%02x from=%d seq=%d ttl=%d rssi=%d",
            hdr.mesh_type, hdr.origin_id, hdr.seq_num, hdr.ttl, rssi);

    /* Privacy filter: only relay PLAYER_STATE from same team */
    bool should_relay = true;
    bool should_forward = true;

    if (hdr.mesh_type == MESH_PLAYER_STATE && hdr.payload_len >= 2) {
        uint8_t msg_team_id = data[MESH_HEADER_SIZE + 1];
        if (msg_team_id != local_team_id) {
            should_relay = false;
            should_forward = false;
        }
    }

    if (should_forward) {
        forward_to_phone(data, len);
    }

    if (should_relay && hdr.ttl > 0) {
        relay_message(data, len);
    }
}

/* Called during init to create the Coded PHY advertising set */
int mesh_create_coded_adv_set(void)
{
    struct bt_le_adv_param adv_param = {
        .id = BT_ID_DEFAULT,
        .sid = 1,
        .secondary_max_skip = 0,
        .options = BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_CODED,
        .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
        .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
        .peer = NULL,
    };

    int err = bt_le_ext_adv_create(&adv_param, NULL, &coded_adv_set);
    if (err) {
        LOG_ERR("Coded PHY adv set create failed (err %d)", err);
    }
    return err;
}
