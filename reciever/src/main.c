#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "protocol.h"
#include "ir_protocol.h"
#include "vest_ble.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ===== Hardware ===== */
#define IR_SENSOR_NODE  DT_ALIAS(irsensor)
#define STATUS_LED_NODE DT_ALIAS(led0)
#define UNPAIR_BTN_NODE DT_ALIAS(unpair_button)

static const struct gpio_dt_spec ir_pin = GPIO_DT_SPEC_GET(IR_SENSOR_NODE, gpios);
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(STATUS_LED_NODE, gpios);

#if DT_NODE_HAS_STATUS(UNPAIR_BTN_NODE, okay)
static const struct gpio_dt_spec unpair_btn = GPIO_DT_SPEC_GET(UNPAIR_BTN_NODE, gpios);
static struct gpio_callback unpair_cb_data;
static bool has_unpair_btn = true;
#else
static bool has_unpair_btn = false;
#endif

/* ===== State ===== */
static enum vest_state current_state = VEST_UNPAIRED;
static struct bt_conn *emitter_conn;
static char adv_name[11] = "LT-V-0000";

/* Pairing - stored emitter MAC in NVS */
static bt_addr_le_t paired_emitter_addr;
static bool has_paired_addr;

/* IR-pairing in flight: vest heard a pairing IR frame and is now
 * advertising, waiting for the emitter named in the IR payload to
 * connect. The connecting peer's BLE address is matched against
 * expected_addr_lsbs in connected(). No GATT round-trip required.
 */
static uint16_t expected_addr_lsbs;       /* IR-supplied low 2 bytes of emitter MAC */
static uint8_t  pending_emitter_player_id;
static bool     pair_pending;
static struct k_work_delayable pair_pending_timeout_work;
#define PAIR_PENDING_TIMEOUT_MS 30000     /* 30 s — plenty of time for emitter to connect */

/* Friendly list */
static uint8_t friendly_ids[MAX_PLAYERS];
static uint8_t friendly_count;
static bool friendly_fire_enabled;

/* Hit retry state */
static struct k_work_delayable hit_retry_work;
static uint8_t pending_hit_buf[VEST_HIT_PAYLOAD_SIZE];
static uint16_t pending_hit_pkt_id;
static bool hit_pending;
static uint8_t hit_retry_count;
#define HIT_RETRY_INTERVAL_MS 50
#define HIT_RETRY_MAX_COUNT   10  /* 50ms * 10 = 500ms timeout */

/* LED */
static struct k_timer led_timer;
static int led_tick;

/* ===== Forward Declarations ===== */
static void start_advertising(void);
static void start_pairing_scan(void);

/* ===== LED ===== */

static void led_timer_handler(struct k_timer *timer_id)
{
    led_tick++;
    /* LED patterns (timer fires every 100ms, so led_tick is 0.1s units):
     *   UNPAIRED, no IR yet     - brief flash every 500ms (1 of 5 ticks)
     *   UNPAIRED, pair_pending  - rapid blink every 200ms (alternating)
     *   PAIRED_IDLE, no link    - heartbeat: two quick flashes per second
     *   PAIRED_IDLE, linked     - solid
     *   GAME_ACTIVE             - solid
     *   DEAD                    - fast alternate every tick
     */
    switch (current_state) {
    case VEST_UNPAIRED:
        if (pair_pending) {
            gpio_pin_set_dt(&status_led, (led_tick % 2 == 0) ? 1 : 0);
        } else {
            gpio_pin_set_dt(&status_led, (led_tick % 5 == 0) ? 1 : 0);
        }
        break;
    case VEST_PAIRED_IDLE: {
        bool linked = (emitter_conn != NULL);
        if (linked) {
            gpio_pin_set_dt(&status_led, 1);
        } else {
            /* Heartbeat: ON for ticks 0 and 2 in every 10-tick window,
             * OFF otherwise. Looks like two short blinks each second.
             */
            uint8_t phase = led_tick % 10;
            gpio_pin_set_dt(&status_led, (phase == 0 || phase == 2) ? 1 : 0);
        }
        break;
    }
    case VEST_GAME_ACTIVE:
        gpio_pin_set_dt(&status_led, 1);
        break;
    case VEST_DEAD:
        gpio_pin_toggle_dt(&status_led);
        break;
    }
}

/* ===== Friendly Fire Check ===== */

static bool is_friendly(uint8_t player_id)
{
    if (friendly_fire_enabled) {
        return false;
    }
    for (int i = 0; i < friendly_count; i++) {
        if (friendly_ids[i] == player_id) {
            return true;
        }
    }
    return false;
}

/* ===== IR Pairing ===== */

static void pair_pending_timeout_handler(struct k_work *work)
{
    if (has_paired_addr || !pair_pending) {
        return;
    }
    LOG_WRN("Pair-pending timeout — no matching emitter connected, reverting to UNPAIRED");
    pair_pending = false;
    if (emitter_conn) {
        bt_conn_disconnect(emitter_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    } else {
        start_pairing_scan();
    }
}

/* IR pairing frame received. Strict policy: only acts when fully
 * unpaired. The frame carries the emitter's own BLE address bytes 0-1;
 * we store them and verify the connecting peer's MAC at connected()
 * time, so no GATT round-trip is needed to confirm pairing identity.
 *
 * If a second IR frame arrives while we're already pair-pending we
 * ignore it — first IR wins. This prevents a neighboring emitter in
 * pairing mode from clobbering the binding we're about to commit.
 */
static void on_ir_pairing(const ir_pairing_packet_t *pkt)
{
    if (current_state != VEST_UNPAIRED) {
        LOG_INF("IR pairing ignored (state=%d) — vest believes it is already paired",
                current_state);
        return;
    }
    if (pair_pending) {
        LOG_DBG("IR pairing ignored — already pair-pending");
        return;
    }

    LOG_INF("IR pairing received: emitter_addr_lsbs=0x%04x emitter_pid=%d",
            pkt->addr_lsbs, pkt->player_id);

    expected_addr_lsbs = pkt->addr_lsbs;
    pending_emitter_player_id = pkt->player_id;
    pair_pending = true;

    /* Start advertising so the emitter can find us by name and connect.
     * In connected() we'll compare the peer's MAC to expected_addr_lsbs
     * and either commit the pairing (match) or disconnect (mismatch).
     */
    start_advertising();
    k_work_reschedule(&pair_pending_timeout_work, K_MSEC(PAIR_PENDING_TIMEOUT_MS));
}

/* ===== Hit Retry ===== */

static void send_hit_notification(void)
{
    if (!emitter_conn) {
        hit_pending = false;
        return;
    }
    vest_ble_notify_hit(emitter_conn, pending_hit_buf, VEST_HIT_PAYLOAD_SIZE);
}

static void hit_retry_handler(struct k_work *work)
{
    if (!hit_pending) {
        return;
    }
    hit_retry_count++;
    if (hit_retry_count >= HIT_RETRY_MAX_COUNT) {
        LOG_WRN("Hit retry timeout, dropping pkt=0x%04x", pending_hit_pkt_id);
        hit_pending = false;
        return;
    }
    send_hit_notification();
    k_work_reschedule(&hit_retry_work, K_MSEC(HIT_RETRY_INTERVAL_MS));
}

/* ===== IR Receive Callback ===== */

static void on_ir_rx(const ir_packet_t *packet, bool crc_valid)
{
    if (!crc_valid) {
        LOG_WRN("IR: CRC fail, dropped");
        return;
    }

    if (current_state != VEST_GAME_ACTIVE) {
        return;
    }

    if (is_friendly(packet->player_id)) {
        LOG_DBG("IR: friendly fire from %d, dropped", packet->player_id);
        return;
    }

    if (hit_pending) {
        LOG_WRN("IR: hit already pending, dropped");
        return;
    }

    uint16_t pkt_id = sys_rand32_get() & 0xFFFF;
    pending_hit_pkt_id = pkt_id;

    sys_put_le16(pkt_id, &pending_hit_buf[0]);
    pending_hit_buf[2] = packet->player_id;
    pending_hit_buf[3] = packet->weapon_id;
    pending_hit_buf[4] = packet->team_id;

    hit_pending = true;
    hit_retry_count = 0;

    LOG_INF("HIT from player %d (weapon=%d team=%d) pkt=0x%04x",
            packet->player_id, packet->weapon_id, packet->team_id, pkt_id);

    send_hit_notification();
    k_work_reschedule(&hit_retry_work, K_MSEC(HIT_RETRY_INTERVAL_MS));
}

/* ===== Emitter Command Handler ===== */

static void on_emitter_cmd(const uint8_t *data, uint16_t len)
{
    if (len == 0) {
        return;
    }

    switch (data[0]) {
    case VEST_CMD_HIT_ACK:
        if (len >= 3) {
            uint16_t ack_pkt_id = sys_get_le16(&data[1]);
            if (hit_pending && ack_pkt_id == pending_hit_pkt_id) {
                hit_pending = false;
                k_work_cancel_delayable(&hit_retry_work);
                LOG_INF("Hit ACK received for pkt=0x%04x", ack_pkt_id);
            }
        }
        break;

    case VEST_CMD_FRIENDLY_LIST:
        if (len >= 2) {
            friendly_count = data[1];
            if (friendly_count > MAX_PLAYERS) {
                friendly_count = MAX_PLAYERS;
            }
            if (len >= 2 + friendly_count) {
                memcpy(friendly_ids, &data[2], friendly_count);
            }
            if (current_state == VEST_PAIRED_IDLE) {
                current_state = VEST_GAME_ACTIVE;
            }
            LOG_INF("Friendly list: %d players, state -> GAME_ACTIVE", friendly_count);
        }
        break;

    case VEST_CMD_DEATH:
        current_state = VEST_DEAD;
        hit_pending = false;
        k_work_cancel_delayable(&hit_retry_work);
        LOG_INF("DEATH: IR hits disabled");
        break;

    case VEST_CMD_RESPAWN:
        current_state = VEST_GAME_ACTIVE;
        LOG_INF("RESPAWN: IR hits re-enabled");
        break;

    case VEST_CMD_FRIENDLY_FIRE:
        if (len >= 2) {
            friendly_fire_enabled = data[1];
            LOG_INF("Friendly fire: %s", friendly_fire_enabled ? "ON" : "OFF");
        }
        break;

    case VEST_CMD_UNPAIR:
        LOG_INF("Unpair command received, clearing NVS");
        settings_delete("vest/emitter_addr");
        has_paired_addr = false;
        pair_pending = false;
        current_state = VEST_UNPAIRED;
        friendly_count = 0;
        hit_pending = false;
        k_work_cancel_delayable(&pair_pending_timeout_work);
        if (emitter_conn) {
            bt_conn_disconnect(emitter_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
        break;
    }
}

/* ===== Unpair Button ===== */
#if DT_NODE_HAS_STATUS(UNPAIR_BTN_NODE, okay)
static void unpair_pressed_cb(const struct device *dev, struct gpio_callback *cb,
                               uint32_t pins)
{
    LOG_INF("Unpair button pressed");

    /* Clear paired state BEFORE the disconnect so the disconnected callback
     * routes to the unpaired branch (scan, not advertise).
     */
    has_paired_addr = false;
    pair_pending = false;
    settings_delete("vest/emitter_addr");
    current_state = VEST_UNPAIRED;
    friendly_count = 0;
    hit_pending = false;
    k_work_cancel_delayable(&pair_pending_timeout_work);

    if (emitter_conn) {
        /* disconnected() will switch us back to pairing scan. */
        bt_conn_disconnect(emitter_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    } else {
        start_pairing_scan();
    }
}
#endif

/* ===== BLE Connection ===== */

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed (err %d)", err);
        return;
    }
    const bt_addr_le_t *peer = bt_conn_get_dst(conn);
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(peer, addr_str, sizeof(addr_str));

    if (has_paired_addr) {
        /* Reconnect path: only the bound emitter is allowed in. */
        if (bt_addr_le_cmp(peer, &paired_emitter_addr) != 0) {
            LOG_WRN("Reject connection from non-paired peer %s", addr_str);
            bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
            return;
        }
        LOG_INF("Emitter reconnected: %s", addr_str);
        emitter_conn = bt_conn_ref(conn);
        return;
    }

    if (pair_pending) {
        /* Initial-pair path: the IR pairing frame named the emitter's
         * BLE address. Verify the connecting peer matches that name
         * before committing. A mismatch means someone else (a stranger
         * emitter, a debug tool) connected during our pair-pending
         * window — reject and stay open for the real emitter.
         */
        uint16_t peer_lsbs = (uint16_t)peer->a.val[0] |
                             ((uint16_t)peer->a.val[1] << 8);
        if (peer_lsbs != expected_addr_lsbs) {
            LOG_WRN("Reject pairing connection from %s (peer_lsbs=0x%04x, expected=0x%04x)",
                    addr_str, peer_lsbs, expected_addr_lsbs);
            bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
            return;
        }

        /* MAC matches — commit the binding (in RAM only; pairing is
         * ephemeral and gets cleared at the next power-on).
         */
        bt_addr_le_copy(&paired_emitter_addr, peer);
        has_paired_addr = true;
        pair_pending = false;
        current_state = VEST_PAIRED_IDLE;
        k_work_cancel_delayable(&pair_pending_timeout_work);
        emitter_conn = bt_conn_ref(conn);
        LOG_INF("Paired via IR-supplied MAC: %s (emitter pid=%d)",
                addr_str, pending_emitter_player_id);
        return;
    }

    /* Not paired and not pair-pending — we shouldn't be advertising in
     * this state. Drop any stray inbound connection.
     */
    LOG_WRN("Unexpected connection in UNPAIRED non-pending state from %s — dropping",
            addr_str);
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Emitter disconnected (reason %d)", reason);
    if (emitter_conn) {
        bt_conn_unref(emitter_conn);
        emitter_conn = NULL;
    }
    hit_pending = false;
    k_work_cancel_delayable(&hit_retry_work);

    if (current_state == VEST_GAME_ACTIVE || current_state == VEST_DEAD) {
        current_state = VEST_PAIRED_IDLE;
    }

    if (has_paired_addr) {
        start_advertising();
    } else if (pair_pending) {
        /* A non-matching peer connected and was rejected — keep
         * advertising so the IR-supplied emitter can still connect
         * within the pair-pending window.
         */
        start_advertising();
    } else {
        /* Unpair-button path lands here: drop back to listening for IR. */
        start_pairing_scan();
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* ===== Advertising / Scanning =====
 *
 * Unpaired vests do NOT advertise — they only scan for the emitter's
 * trigger-pull pairing broadcast. Otherwise the emitter (which discovers
 * vests by name) would auto-connect to any unpaired vest in range without
 * the pairing dance, and without storing the emitter↔vest binding.
 *
 * Once paired, the vest advertises connectable+scannable with its name so
 * the emitter can find it. True directed advertising would be preferable
 * here, but legacy directed adv carries no payload and the emitter relies
 * on the "LT-V-" name prefix to identify vests, so we use undirected.
 */

static void start_advertising(void)
{
    bt_le_adv_stop();
    bt_le_scan_stop();

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
    } else {
        LOG_INF("Advertising started (paired)");
    }
}

/* Vest enters this state when fully unpaired. We don't advertise (the
 * emitter would happily connect by name without any pairing dance) and
 * we don't BLE-scan (pairing arrives over IR). The IR receiver loop is
 * always running in main(), so pairing reception is implicit.
 */
static void start_pairing_scan(void)
{
    bt_le_adv_stop();
    LOG_INF("Pairing-listen mode (unpaired, waiting for IR pairing frame)");
}

/* ===== BT Ready ===== */

static void bt_ready(int err)
{
    if (err) {
        LOG_ERR("BT init failed (err %d)", err);
        return;
    }

    /* Set device name: LT-V-XXXX */
    bt_addr_le_t addrs[1];
    size_t count = 1;
    bt_id_get(addrs, &count);
    if (count > 0) {
        snprintf(adv_name, sizeof(adv_name), "LT-V-%02X%02X",
                 addrs[0].a.val[1], addrs[0].a.val[0]);
    }
    bt_set_name(adv_name);
    LOG_INF("Device name: %s", adv_name);

    if (has_paired_addr) {
        LOG_INF("Have paired emitter, advertising for reconnect");
        start_advertising();
    } else {
        LOG_INF("No paired emitter, scanning for pairing broadcast");
        start_pairing_scan();
    }
}

/* ===== Settings (NVS) ===== */

static int settings_set(const char *name, size_t len,
                        settings_read_cb read_cb, void *cb_arg)
{
    if (strcmp(name, "emitter_addr") == 0 && len == sizeof(paired_emitter_addr)) {
        read_cb(cb_arg, &paired_emitter_addr, sizeof(paired_emitter_addr));
        has_paired_addr = true;
        current_state = VEST_PAIRED_IDLE;
        char addr_str[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(&paired_emitter_addr, addr_str, sizeof(addr_str));
        LOG_INF("NVS: loaded emitter addr: %s", addr_str);
        return 0;
    }
    return -ENOENT;
}

static struct settings_handler settings_h = {
    .name = "vest",
    .h_set = settings_set,
};

/* ===== Main ===== */

int main(void)
{
    LOG_INF("Laser Tag Vest starting...");

    /* LED */
    gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
    k_timer_init(&led_timer, led_timer_handler, NULL);
    k_timer_start(&led_timer, K_MSEC(100), K_MSEC(100));

    /* Unpair button */
#if DT_NODE_HAS_STATUS(UNPAIR_BTN_NODE, okay)
    if (has_unpair_btn) {
        gpio_pin_configure_dt(&unpair_btn, GPIO_INPUT);
        gpio_pin_interrupt_configure_dt(&unpair_btn, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&unpair_cb_data, unpair_pressed_cb, BIT(unpair_btn.pin));
        gpio_add_callback(unpair_btn.port, &unpair_cb_data);
    }
#endif

    /* Hit retry work */
    k_work_init_delayable(&hit_retry_work, hit_retry_handler);

    /* Pair-pending timeout (reverts to UNPAIRED if no matching emitter connects) */
    k_work_init_delayable(&pair_pending_timeout_work, pair_pending_timeout_handler);

    /* NVS settings.
     *
     * Ephemeral pairing: we deliberately do NOT call settings_load() and
     * we wipe any persisted vest/emitter_addr entry at boot. Reasoning:
     * the Pro Micro is flashed via UF2 (drag-and-drop bootloader), which
     * can't erase NVS along with the app region. If a stale pairing got
     * stuck in NVS, there'd be no way to clear it short of an SWD
     * --erase. So pairing is now fresh on every power-on — re-pair by
     * shooting the TSOP, which takes a second anyway.
     */
    settings_subsys_init();
    settings_register(&settings_h);
    settings_delete("vest/emitter_addr");

    /* Vest BLE service */
    vest_ble_init(on_emitter_cmd);

    /* IR receiver */
    if (ir_rx_init(&ir_pin, on_ir_rx) < 0) {
        LOG_ERR("IR sensor init failed");
        return 0;
    }
    ir_rx_set_pairing_cb(on_ir_pairing);

    /* Bluetooth */
    int err = bt_enable(bt_ready);
    if (err) {
        LOG_ERR("BT enable failed (err %d)", err);
    }

    /* Main loop: process IR decoder */
    while (1) {
        ir_rx_loop();
        k_msleep(10);
    }

    return 0;
}
