#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

/* ===== Phone -> Emitter Opcodes (write to RX) ===== */
#define CMD_CONFIG              0x01
#define CMD_START               0x02
#define CMD_AMMO_INCREASE       0x03
#define CMD_GAME_OVER           0x04
#define CMD_BROADCAST_CODED_PHY 0x05
#define CMD_SET_PLAYER_INFO     0x10
#define CMD_START_LOBBY_HOST    0x11
#define CMD_START_LOBBY_SCAN    0x12
#define CMD_STOP_LOBBY          0x13
#define CMD_BROADCAST_GAME_CFG  0x14
#define CMD_UPDATE_LOCATION     0x15
#define CMD_REQUEST_MESH_STATS  0x16
#define CMD_ENTER_PAIRING       0x17
#define CMD_UNPAIR_VEST         0x18
#define CMD_RESPAWN             0x19
#define CMD_RELOAD              0x1A

/* ===== Emitter -> Phone Opcodes (notify on TX) ===== */
#define RSP_CONFIG_ACK          0x81
#define RSP_START_ACK           0x82
#define RSP_AMMO_ACK            0x83
#define RSP_GAME_OVER_ACK       0x84
#define RSP_BROADCAST_ACK       0x85
#define RSP_PLAYER_INFO_ACK     0x90
#define RSP_LOBBY_HOST_ACK      0x91
#define RSP_LOBBY_SCAN_ACK      0x92
#define RSP_GAME_CFG_ACK        0x94
#define RSP_LOCATION_ACK        0x95
#define RSP_MESH_STATS_ACK      0x96
#define RSP_PAIRING_ACK         0x97
#define RSP_VEST_PAIRED         0x98
#define RSP_RESPAWN_ACK         0x99
#define RSP_STATE_UPDATE        0xA0
#define RSP_DEATH_NOTIFY        0xA1

/* ===== Emitter -> Vest Opcodes (write to vest Command RX) ===== */
#define VEST_CMD_HIT_ACK        0x01
#define VEST_CMD_FRIENDLY_LIST  0x02
#define VEST_CMD_DEATH          0x03
#define VEST_CMD_RESPAWN        0x04
#define VEST_CMD_FRIENDLY_FIRE  0x05

/* ===== Mesh Message Types ===== */
#define MESH_LOBBY_ANNOUNCE     0x01
#define MESH_LOBBY_JOIN         0x02
#define MESH_LOBBY_ACCEPT       0x03
#define MESH_LOBBY_STATE        0x04
#define MESH_GAME_START         0x05
#define MESH_PLAYER_STATE       0x10
#define MESH_HIT_EVENT          0x11
#define MESH_PLAYER_DEATH       0x12
#define MESH_GAME_END           0x13
#define MESH_SCOREBOARD         0x14
#define MESH_PLAYER_RESPAWN     0x15

/* ===== Mesh Constants ===== */
#define MESH_HEADER_SIZE        6
#define MESH_DEFAULT_TTL        3
#define MESH_DEDUP_SIZE         64
#define MESH_RELAY_JITTER_MIN   5
#define MESH_RELAY_JITTER_MAX   30

/* ===== Emitter Configuration (14 bytes, packed) ===== */
struct emitter_config {
    uint8_t  user_id;
    uint8_t  damage;
    uint16_t mag_size;
    uint16_t fire_rate_ms;
    uint16_t reload_speed_ms;
    uint8_t  full_auto;
    uint8_t  ammo_type;
    uint16_t initial_total_ammo;
    uint8_t  max_health;
    uint8_t  friendly_fire;
} __attribute__((packed));

#define EMITTER_CONFIG_SIZE 14

/* ===== Mesh Header (6 bytes, packed) ===== */
struct mesh_header {
    uint8_t  mesh_type;
    uint8_t  origin_id;
    uint16_t seq_num;
    uint8_t  ttl;
    uint8_t  payload_len;
} __attribute__((packed));

/* ===== Emitter State Machine ===== */
enum emitter_state {
    EMITTER_IDLE,
    EMITTER_PAIRING,
    EMITTER_LOBBY_HOST,
    EMITTER_LOBBY_SCAN,
    EMITTER_GAME_ACTIVE,
    EMITTER_DEAD,
    EMITTER_GAME_OVER,
};

/* ===== Vest State Machine ===== */
enum vest_state {
    VEST_UNPAIRED,
    VEST_PAIRED_IDLE,
    VEST_GAME_ACTIVE,
    VEST_DEAD,
};

/* ===== Player Identity ===== */
#define MAX_USERNAME_LEN 16
#define MAX_PLAYERS      16
#define MAX_TEAMS        6

struct player_identity {
    uint8_t player_id;
    uint8_t team_id;
    char    username[MAX_USERNAME_LEN + 1];
};

/* ===== Hit Dedup Ring Buffer Entry ===== */
struct hit_dedup_entry {
    uint16_t packet_id;
    bool     used;
};

#define HIT_DEDUP_SIZE 16

/* ===== Vest Hit TX Payload ===== */
struct vest_hit_payload {
    uint16_t packet_id;
    uint8_t  shooter_id;
    uint8_t  weapon_id;
    uint8_t  team_id;
} __attribute__((packed));

#define VEST_HIT_PAYLOAD_SIZE 5

/* ===== Pairing Advertisement ===== */
#define PAIRING_FLAG 0xAA
#define PAIRING_ADV_SIZE 8

#endif /* PROTOCOL_H */
