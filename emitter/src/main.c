#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <stdio.h>

#include "ble_gatt.h"
#include "ir_handler.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ----- State Machine & Variables ----- */
enum emitter_state { STATE_UNCONFIGURED, STATE_CONFIGURED, STATE_ACTIVE };
enum led_state { LED_DISCONNECTED, LED_FLASHING_ID, LED_CONNECTED_SOLID };

struct emitter_config {
    uint8_t user_id;
    uint8_t damage;
    uint16_t mag_size;
    uint16_t fire_rate_ms;
    uint16_t reload_speed_ms;
    uint8_t full_auto;
    uint8_t ammo_type;
    uint16_t initial_total_ammo;
};

static struct emitter_config emitter_config;
static enum emitter_state current_state = STATE_UNCONFIGURED;

// Ammo Tracking
static uint16_t current_mag_ammo = 0;
static uint16_t current_total_ammo = 0;

static atomic_t config_loaded = ATOMIC_INIT(0);
static struct bt_conn *current_conn;

// Persistence and LED Variables
static bt_addr_le_t bound_peer_addr;
static bool is_bound = false;
static enum led_state current_led_state = LED_DISCONNECTED;
static uint8_t connection_id_blinks = 1;
static int flash_count_remaining = 0;
static char adv_name[16] = "emitter";

// Mechanics
static bool trigger_ready = true; 
static bool is_reloading = false;

/* ----- BLE Service ----- */
// See common/include/ble_gatt.h for GATT service definitions

// Forward Declarations for Timers
static struct k_work_delayable trigger_work; 
static struct k_work_delayable reload_work;
static struct k_work_delayable adv_work;

/* ----- Coded PHY Variables & Callbacks ----- */
static struct bt_le_ext_adv *coded_adv_set;
static struct k_work_delayable stop_coded_adv_work;
static atomic_t is_broadcasting = ATOMIC_INIT(0);

static struct bt_le_scan_param scan_param = {
    .type       = BT_LE_SCAN_TYPE_PASSIVE,
    .options    = BT_LE_SCAN_OPT_CODED | BT_LE_SCAN_OPT_NO_1M,
    .interval   = 0x00A0,
    .window     = 0x00A0,
};

static void send_notification(uint8_t *data, uint16_t len);

static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
    LOG_INF("Received Coded PHY hit (or msg)! RSSI: %d dBm", info->rssi);
    LOG_HEXDUMP_INF(buf->data, buf->len, "Coded PHY RX:");

    if (current_conn && buf->len > 0) {
        uint8_t notify_data[buf->len + 1];
        notify_data[0] = 0x05;
        for (int i = 0; i < buf->len; i++) {
            notify_data[i + 1] = buf->data[i];
        }
        send_notification(notify_data, buf->len + 1);
    }
}

static struct bt_le_scan_cb scan_callbacks = {
    .recv = scan_recv,
};

static void stop_coded_adv_handler(struct k_work *work)
{
    bt_le_ext_adv_stop(coded_adv_set);
    atomic_set(&is_broadcasting, 0);

    /* Safely restart scanner */
    int err = bt_le_scan_start(&scan_param, NULL);
    if (err && err != -EALREADY) {
        LOG_ERR("Failed to restart Coded PHY scanner (err %d)", err);
    } else {
        LOG_INF("Coded PHY Scanner restarted.");
    }
}

static void send_notification(uint8_t *data, uint16_t len) {
    ble_gatt_notify(current_conn, data, len);
}

static void on_ble_gatt_rx(const uint8_t *data, uint16_t len) {
    if (len == 0) return;
    uint8_t res[1];

    switch (data[0]) {
        case 0x01: // CMD_CONFIG
            if (len >= 13) {
                emitter_config.user_id = data[1];
                emitter_config.damage = data[2];
                emitter_config.mag_size = sys_get_le16(&data[3]);
                emitter_config.fire_rate_ms = sys_get_le16(&data[5]);
                emitter_config.reload_speed_ms = sys_get_le16(&data[7]);
                emitter_config.full_auto = data[9];
                emitter_config.ammo_type = data[10];
                emitter_config.initial_total_ammo = sys_get_le16(&data[11]);

                settings_save_one("emitter/config", &emitter_config, sizeof(emitter_config));
                current_state = STATE_CONFIGURED;
                atomic_set(&config_loaded, 1);
                
                LOG_INF("Config: Mag:%d, Reload:%dms, Auto:%d, AmmoType:%d, TotalAmmo:%d", 
                        emitter_config.mag_size, emitter_config.reload_speed_ms, emitter_config.full_auto,
                        emitter_config.ammo_type, emitter_config.initial_total_ammo);
                        
                res[0] = 0x81; send_notification(res, 1); 
            } else {
                LOG_WRN("Config payload too short! Need 13 bytes.");
            }
            break;

        case 0x02: // CMD_START
            if (current_state == STATE_CONFIGURED) {
                current_state = STATE_ACTIVE;
                is_reloading = false;

                // Divide the starting ammo between the Magazine and the Reserve
                uint16_t starting_mag = (emitter_config.initial_total_ammo > emitter_config.mag_size) ? 
                                         emitter_config.mag_size : emitter_config.initial_total_ammo;
                
                current_mag_ammo = starting_mag;
                current_total_ammo = emitter_config.initial_total_ammo - starting_mag;

                LOG_INF("Game Started! Mag: %d, Reserve: %d", current_mag_ammo, current_total_ammo);
                res[0] = 0x82; send_notification(res, 1);
            }
            break;

        case 0x03: // CMD_AMMO_INCREASE (e.g. 0x03 32 00 -> Adds 50 ammo)
            if (current_state == STATE_ACTIVE && len >= 3) {
                uint16_t ammo_to_add = sys_get_le16(&data[1]);
                current_total_ammo += ammo_to_add;
                LOG_INF("Ammo Pack Received! Added %d. Total Reserve: %d", ammo_to_add, current_total_ammo);
                res[0] = 0x83; send_notification(res, 1);
            }
            break;

        case 0x04: // CMD_GAMEOVER
            current_state = STATE_CONFIGURED;
            is_bound = false; 
            is_reloading = false;
            k_work_cancel_delayable(&reload_work);
            k_work_cancel_delayable(&trigger_work);
            LOG_INF("Game Over. Session Unbound.");
            res[0] = 0x84; send_notification(res, 1);
            break;

        case 0x05: // CMD_BROADCAST_CODED_PHY
            if (len > 1) {
                uint8_t payload_len = len - 1;
                struct bt_data ad[] = {
                    BT_DATA(BT_DATA_MANUFACTURER_DATA, &data[1], payload_len)
                };

                // Stop scanning before broadcasting to avoid receiving our own message
                bt_le_scan_stop();

                int err = bt_le_ext_adv_set_data(coded_adv_set, ad, ARRAY_SIZE(ad), NULL, 0);
                if (err) {
                    LOG_ERR("Failed to set Coded PHY payload (err %d)", err);
                }

                struct bt_le_ext_adv_start_param start_param = {
                    .timeout = 0,
                    .num_events = 1,
                };
                err = bt_le_ext_adv_start(coded_adv_set, &start_param);
                if (err) {
                    LOG_ERR("Failed to start Coded PHY adv (err %d)", err);
                } else {
                    LOG_INF("Broadcasting %d bytes on Coded PHY. Scanning will resume in 500ms.", payload_len);
                    atomic_set(&is_broadcasting, 1);
                    k_work_reschedule(&stop_coded_adv_work, K_MSEC(500));
                }

                res[0] = 0x85; send_notification(res, 1);
            } else {
                LOG_WRN("Broadcast command has no payload.");
            }
            break;
    }
}

/* ----- Hardware Aliases ----- */
#define TRIGGER_NODE DT_ALIAS(trigger_button)
#define PWM_IR_LED_NODE DT_ALIAS(ir_pwm)
#define STATUS_LED_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec trigger = GPIO_DT_SPEC_GET(TRIGGER_NODE, gpios);
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(STATUS_LED_NODE, gpios);
static const struct device *pwm_dev = DEVICE_DT_GET(PWM_IR_LED_NODE);
static struct gpio_callback trigger_cb_data;

#define IR_CARRIER_PERIOD_US 26
static struct k_work ir_tx_work;
static int64_t last_shot_time = 0;

/* ----- Status LED Timer ----- */
static struct k_timer led_timer;
static int led_tick = 0;

static void led_timer_handler(struct k_timer *timer_id) {
    led_tick++;
    if (current_led_state == LED_DISCONNECTED) {
        gpio_pin_set_dt(&status_led, (led_tick % 10 == 0) ? 1 : 0);
    } 
    else if (current_led_state == LED_FLASHING_ID) {
        if (flash_count_remaining > 0) {
            gpio_pin_toggle_dt(&status_led);
            flash_count_remaining--;
        } else {
            current_led_state = LED_CONNECTED_SOLID;
            gpio_pin_set_dt(&status_led, 1);
        }
    }
}

/* ----- Shooting & Reloading Logic ----- */

static void reload_work_handler(struct k_work *work) {
    // Calculate how much we need to fill the mag
    uint16_t needed = emitter_config.mag_size - current_mag_ammo;
    
    // Take from reserve (take 'needed', unless reserve is smaller than 'needed')
    uint16_t load_amount = (current_total_ammo >= needed) ? needed : current_total_ammo;

    current_mag_ammo += load_amount;
    current_total_ammo -= load_amount;
    
    is_reloading = false;
    trigger_ready = false; // Require physical release of trigger before firing again
    
    LOG_INF("Reload Complete! Mag: %d, Reserve: %d", current_mag_ammo, current_total_ammo);
    
    uint8_t ammo_evt[5];
    ammo_evt[0] = 0x06; // Ammo update event
    sys_put_le16(current_mag_ammo, &ammo_evt[1]);
    sys_put_le16(current_total_ammo, &ammo_evt[3]);
    send_notification(ammo_evt, sizeof(ammo_evt));
}

static void ir_tx_work_handler(struct k_work *work) {
    ir_packet_t pkt = { .user_id = emitter_config.user_id, .damage = emitter_config.damage };
    ir_handler_tx_send(&pkt);
}

static void trigger_work_handler(struct k_work *work) {
    bool is_pressed = (gpio_pin_get_dt(&trigger) == 1);

    if (!is_pressed) {
        trigger_ready = true;
        return; 
    }

    if (current_state != STATE_ACTIVE) return;
    if (is_reloading) return; 

    // EMPTY MAG / RELOAD LOGIC
    if (current_mag_ammo == 0) {
        if (trigger_ready) { // Only attempt reload on a fresh trigger pull
            if (current_total_ammo > 0) {
                LOG_INF("Mag Empty! Reloading... (%d ms)", emitter_config.reload_speed_ms);
                is_reloading = true;
                trigger_ready = false;
                k_work_reschedule(&reload_work, K_MSEC(emitter_config.reload_speed_ms));
            } else {
                LOG_INF("OUT OF AMMO! Need an ammo pack.");
                trigger_ready = false; // Prevent spamming the log
            }
        }
        return; 
    }

    // SEMI-AUTO CHECK
    if (!emitter_config.full_auto && !trigger_ready) {
        return; 
    }

    // FIRE RATE CHECK
    int64_t now = k_uptime_get();
    int64_t elapsed = now - last_shot_time;
    if (elapsed < emitter_config.fire_rate_ms) {
        int64_t wait_time = emitter_config.fire_rate_ms - elapsed;
        k_work_reschedule(&trigger_work, K_MSEC(wait_time));
        return;
    }

    // FIRE!
    last_shot_time = now;
    current_mag_ammo--;
    trigger_ready = false;
    LOG_INF("FIRE! Mag: %d, Reserve: %d", current_mag_ammo, current_total_ammo);
    
    uint8_t ammo_evt[5];
    ammo_evt[0] = 0x06; // Ammo update event
    sys_put_le16(current_mag_ammo, &ammo_evt[1]);
    sys_put_le16(current_total_ammo, &ammo_evt[3]);
    send_notification(ammo_evt, sizeof(ammo_evt));

    k_work_submit(&ir_tx_work);

    // AUTO-FIRE LOOP
    if (emitter_config.full_auto && current_mag_ammo > 0) {
        k_work_reschedule(&trigger_work, K_MSEC(emitter_config.fire_rate_ms));
    }
}

void trigger_pressed_cb(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    k_work_reschedule(&trigger_work, K_NO_WAIT);
}

/* ----- Bluetooth ----- */
static void start_advertising(void) {
    struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, strlen(adv_name)),
    };
    struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_SCANNABLE,
        BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);
    
    int err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        if (err == -EALREADY) {
            LOG_WRN("Advertising is already running.");
        } else {
            LOG_ERR("Advertising failed to start (err %d)", err);
        }
    } else {
        LOG_INF("Advertising successfully re-started.");
    }
}

static void adv_work_handler(struct k_work *work) {
    start_advertising();
}

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        LOG_ERR("Connection failed (err 0x%02x)", err);
        return;
    }
    
    const bt_addr_le_t *peer_addr = bt_conn_get_dst(conn);
    char addr_str[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(peer_addr, addr_str, sizeof(addr_str));

    LOG_INF("Connection attempt from: %s", addr_str);

    if (is_bound) {
        if (bt_addr_le_cmp(peer_addr, &bound_peer_addr) != 0) {
            LOG_WRN("REJECTED: Phone MAC doesn't match bound session!");
            bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
            return;
        }
    } else {
        bound_peer_addr = *peer_addr;
        is_bound = true;
        connection_id_blinks = (bound_peer_addr.a.val[0] % 5) + 1;
        LOG_INF("Session Bound to this phone.");
    }

    current_led_state = LED_FLASHING_ID;
    flash_count_remaining = connection_id_blinks * 2; 
    led_tick = 0; 
    current_conn = bt_conn_ref(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    current_led_state = LED_DISCONNECTED;
    if (current_conn) { 
        bt_conn_unref(current_conn); 
        current_conn = NULL; 
    }
    
    LOG_INF("Disconnected. Restarting advertising in 100ms...");
    
    // --- CHANGED: Give the Bluetooth stack 100ms to clean up its memory! ---
    k_work_reschedule(&adv_work, K_MSEC(100)); 
}

BT_CONN_CB_DEFINE(conn_callbacks) = { .connected = connected, .disconnected = disconnected };

static void bt_ready(int err) {
    if (err) {
        LOG_ERR("Bluetooth initialization failed (err %d)", err);
        return;
    }

    bt_addr_le_t addrs[1];
    size_t count = 1;
    bt_id_get(addrs, &count);
    if (count > 0) {
        sprintf(&adv_name[7], "%02X%02X%02X", addrs[0].a.val[2], addrs[0].a.val[1], addrs[0].a.val[0]);
    }
    start_advertising();

    /* --- Coded PHY Ext Advertising & Scanner Setup --- */
    struct bt_le_adv_param adv_param = {
        .id = BT_ID_DEFAULT,
        .sid = 1, // Must be 1 to not conflict with legacy ad (which defaults to 0)
        .secondary_max_skip = 0,
        .options = BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_CODED,
        .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
        .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
        .peer = NULL,
    };

    err = bt_le_ext_adv_create(&adv_param, NULL, &coded_adv_set);
    if (err) {
        LOG_ERR("Failed to create Coded PHY Adv set (err %d)", err);
    }

    bt_le_scan_cb_register(&scan_callbacks);
    err = bt_le_scan_start(&scan_param, NULL);
    if (err) {
        LOG_ERR("Failed to start Coded PHY scanner (err %d)", err);
    } else {
        LOG_INF("Coded PHY scanner started successfully.");
    }
}

/* ----- Settings ----- */
static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (strcmp(name, "config") == 0) {
        if (len == sizeof(emitter_config)) {
            read_cb(cb_arg, &emitter_config, sizeof(emitter_config));
            atomic_set(&config_loaded, 1);
            current_state = STATE_CONFIGURED;
            LOG_INF("Loaded config from memory.");
        }
        return 0;
    }
    return -ENOENT;
}
struct settings_handler h = { .name = "emitter", .h_set = settings_set };

int main(void) {
    gpio_pin_configure_dt(&trigger, GPIO_INPUT);
    gpio_pin_interrupt_configure_dt(&trigger, GPIO_INT_EDGE_BOTH);
    gpio_init_callback(&trigger_cb_data, trigger_pressed_cb, BIT(trigger.pin));
    gpio_add_callback(trigger.port, &trigger_cb_data);

    gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);

    k_work_init_delayable(&trigger_work, trigger_work_handler);
    k_work_init_delayable(&reload_work, reload_work_handler);
    k_work_init_delayable(&adv_work, adv_work_handler);
    k_work_init_delayable(&stop_coded_adv_work, stop_coded_adv_handler);
    k_work_init(&ir_tx_work, ir_tx_work_handler);

    k_timer_init(&led_timer, led_timer_handler, NULL);
    k_timer_start(&led_timer, K_MSEC(100), K_MSEC(100));

    ir_handler_tx_init(pwm_dev, IR_CARRIER_PERIOD_US);

    settings_subsys_init();
    settings_register(&h);
    settings_load();
    
    ble_gatt_init(on_ble_gatt_rx);
    bt_enable(bt_ready);
    return 0;
}