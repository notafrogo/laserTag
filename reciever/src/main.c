#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>

#include "ir_handler.h"
#include "ble_gatt.h"

#define IR_SENSOR_NODE DT_ALIAS(irsensor)
static const struct gpio_dt_spec ir_pin = GPIO_DT_SPEC_GET(IR_SENSOR_NODE, gpios);

static struct bt_conn *current_conn = NULL;

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        printk("Connection failed (err %u)\n", err);
        return;
    }
    printk("Phone Connected!\n");
    current_conn = bt_conn_ref(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    printk("Disconnected (reason %u)\n", reason);
    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    
    // Restart advertising
    struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_SCANNABLE, BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);
    bt_le_adv_start(&param, NULL, 0, NULL, 0);
}

BT_CONN_CB_DEFINE(conn_callbacks) = { .connected = connected, .disconnected = disconnected };

static void bt_ready(int err) {
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return;
    }
    printk("Bluetooth initialized\n");

    struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_NAME_COMPLETE, "lasertag-rx", 11),
    };

    struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_SCANNABLE, BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);
    err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        printk("Advertising failed to start (err %d)\n", err);
        return;
    }
    printk("Advertising started\n");
}

void on_ble_gatt_rx(const uint8_t *data, uint16_t len) {
    // Handle incoming commands from the phone if necessary
}

void on_ir_rx(const ir_packet_t *packet, bool valid) {
    if (valid) {
        printk("\n>>> HIT DETECTED! <<<\n");
        printk("Shooter ID : %d\n", packet->user_id);
        printk("Damage     : %d\n", packet->damage);
        printk("Integrity  : VALID\n");

        if (current_conn) {
            uint8_t hit_payload[3] = { 0x01, packet->user_id, packet->damage };
            ble_gatt_notify(current_conn, hit_payload, sizeof(hit_payload));
            printk("Sent hit to phone!\n");
        }
    } else {
        printk("\n>>> HIT DETECTED! <<<\n");
        printk("Shooter ID : %d\n", packet->user_id);
        printk("Damage     : %d\n", packet->damage);
        printk("Integrity  : INVALID\n");
    }
    printk("-----------------------\n");
}

int main(void)
{
    printk("Starting Laser Tag IR Decoder...\n");

    ble_gatt_init(on_ble_gatt_rx);
    int err = bt_enable(bt_ready);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
    }

    if (ir_handler_rx_init(&ir_pin, on_ir_rx) < 0) {
        printk("Error: IR sensor not ready or could not be configured\n");
        return 0;
    }

    printk("Ready. Waiting to be shot...\n");

    while (1) {
        ir_handler_rx_loop();
        /* Yield thread */
        k_msleep(10);
    }

    return 0;
}