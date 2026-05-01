#include "ir_handler.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/printk.h>

/* --- TX State --- */
static const struct device *tx_pwm = NULL;
static uint32_t tx_carrier_period = 26; // Default to ~38kHz

void ir_handler_tx_init(const struct device *pwm_dev, uint32_t carrier_period_us)
{
    tx_pwm = pwm_dev;
    tx_carrier_period = carrier_period_us;
}

void ir_handler_tx_send(const ir_packet_t *packet)
{
    if (!tx_pwm) return;

    uint8_t buffer[3] = { packet->user_id, packet->damage, 0 };
    buffer[2] = buffer[0] ^ buffer[1]; // Checksum

    // Send the IR burst!
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 8; j++) {
            bool bit = (buffer[i] >> j) & 1;
            pwm_set_cycles(tx_pwm, 0, tx_carrier_period, tx_carrier_period / 2, 0);
            k_msleep(1);
            pwm_set_cycles(tx_pwm, 0, tx_carrier_period, 0, 0);
            k_msleep(bit ? 3 : 1);
        }
    }
    pwm_set_cycles(tx_pwm, 0, tx_carrier_period, 0, 0);
}

/* --- RX State --- */
typedef enum {
    STATE_IDLE,
    STATE_MARK,
    STATE_SPACE
} rx_state_t;

static volatile rx_state_t rx_state = STATE_IDLE;
static volatile uint32_t last_cycles = 0;
static volatile uint8_t bit_count = 0;
static volatile uint32_t ir_data_buffer = 0;

static volatile uint32_t decoded_data = 0;
static volatile bool data_ready = false;

static ir_handler_rx_cb_t app_rx_cb = NULL;
static struct gpio_callback ir_cb_data;
static const struct gpio_dt_spec *rx_ir_pin = NULL;

static void ir_triggered(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    uint32_t now_cycles = k_cycle_get_32();
    uint32_t delta_us = k_cyc_to_us_floor32(now_cycles - last_cycles);
    last_cycles = now_cycles;

    if (!rx_ir_pin) return;
    int val = gpio_pin_get_dt(rx_ir_pin);

    if (val == 1) {
        if (rx_state == STATE_IDLE) {
            rx_state = STATE_MARK;
            bit_count = 0;
            ir_data_buffer = 0;
        } 
        else if (rx_state == STATE_SPACE) {
            if (delta_us >= 500 && delta_us < 2200) {
                bit_count++;
                rx_state = STATE_MARK;
            } 
            else if (delta_us >= 2200 && delta_us < 5000) {
                ir_data_buffer |= (1UL << bit_count);
                bit_count++;
                rx_state = STATE_MARK;
            } 
            else {
                rx_state = STATE_IDLE;
            }

            if (bit_count == 24) {
                decoded_data = ir_data_buffer;
                data_ready = true;
                rx_state = STATE_IDLE;
            }
        }
    } 
    else {
        if (rx_state == STATE_MARK) {
            if (delta_us >= 500 && delta_us < 2500) {
                rx_state = STATE_SPACE;
            } else {
                rx_state = STATE_IDLE;
            }
        }
    }
}

int ir_handler_rx_init(const struct gpio_dt_spec *ir_pin, ir_handler_rx_cb_t rx_cb)
{
    rx_ir_pin = ir_pin;
    app_rx_cb = rx_cb;

    if (!gpio_is_ready_dt(rx_ir_pin)) {
        return -1;
    }

    int ret = gpio_pin_configure_dt(rx_ir_pin, GPIO_INPUT);
    if (ret) return ret;

    ret = gpio_pin_interrupt_configure_dt(rx_ir_pin, GPIO_INT_EDGE_BOTH);
    if (ret) return ret;
    
    gpio_init_callback(&ir_cb_data, ir_triggered, BIT(rx_ir_pin->pin));
    gpio_add_callback(rx_ir_pin->port, &ir_cb_data);

    return 0;
}

void ir_handler_rx_loop(void)
{
    unsigned int key = irq_lock();
    rx_state_t current_state = rx_state;
    uint32_t cycles_since = k_cycle_get_32() - last_cycles;
    uint8_t current_bits = bit_count;
    uint32_t current_data = ir_data_buffer;
    irq_unlock(key);

    if (current_state == STATE_SPACE) {
        uint32_t delta_us = k_cyc_to_us_floor32(cycles_since);
        if (delta_us > 10000) { 
            if (current_bits == 23) {
                uint8_t rx_user_id = current_data & 0xFF;
                uint8_t rx_damage  = (current_data >> 8) & 0xFF;
                uint8_t rx_partial_chk = (current_data >> 16) & 0x7F; 
                
                uint8_t expected_chk = rx_user_id ^ rx_damage;
                if ((expected_chk & 0x7F) == rx_partial_chk) {
                    uint8_t missing_bit = (expected_chk >> 7) & 1;
                    current_data |= (missing_bit << 23);
                    decoded_data = current_data;
                    data_ready = true;
                }
            }
            rx_state = STATE_IDLE; 
        }
    }

    if (data_ready) {
        uint8_t user_id  = decoded_data & 0xFF;
        uint8_t damage   = (decoded_data >> 8) & 0xFF;
        uint8_t checksum = (decoded_data >> 16) & 0xFF;

        bool valid = (checksum == (user_id ^ damage));

        if (app_rx_cb) {
            ir_packet_t pkt = { .user_id = user_id, .damage = damage };
            app_rx_cb(&pkt, valid);
        }

        data_ready = false;
    }
}