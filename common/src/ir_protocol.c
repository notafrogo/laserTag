#include "ir_protocol.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>

/* ===== CRC-8 (polynomial 0x07) ===== */

uint8_t crc8_calc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* ===== TX ===== */

static const struct device *tx_pwm;
static uint32_t tx_carrier_period = 26;

void ir_tx_init(const struct device *pwm_dev, uint32_t carrier_period_us)
{
    tx_pwm = pwm_dev;
    tx_carrier_period = carrier_period_us;
}

void ir_tx_send(const ir_packet_t *packet)
{
    if (!tx_pwm) {
        return;
    }

    uint8_t raw[3] = { packet->player_id, packet->weapon_id, packet->team_id };
    uint8_t buffer[4] = { raw[0], raw[1], raw[2], crc8_calc(raw, 3) };

    for (int i = 0; i < 4; i++) {
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

/* ===== RX ===== */

typedef enum { RX_IDLE, RX_MARK, RX_SPACE } rx_state_t;

static volatile rx_state_t rx_state = RX_IDLE;
static volatile uint32_t last_cycles;
static volatile uint8_t bit_count;
static volatile uint32_t ir_data_buffer;
static volatile uint32_t decoded_data;
static volatile bool data_ready;

static ir_rx_cb_t app_rx_cb;
static struct gpio_callback ir_cb_data;
static const struct gpio_dt_spec *rx_ir_pin;

static void ir_triggered(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    uint32_t now = k_cycle_get_32();
    uint32_t delta_us = k_cyc_to_us_floor32(now - last_cycles);
    last_cycles = now;

    if (!rx_ir_pin) {
        return;
    }
    int val = gpio_pin_get_dt(rx_ir_pin);

    if (val == 1) {
        if (rx_state == RX_IDLE) {
            rx_state = RX_MARK;
            bit_count = 0;
            ir_data_buffer = 0;
        } else if (rx_state == RX_SPACE) {
            if (delta_us >= 500 && delta_us < 2200) {
                bit_count++;
                rx_state = RX_MARK;
            } else if (delta_us >= 2200 && delta_us < 5000) {
                ir_data_buffer |= (1UL << bit_count);
                bit_count++;
                rx_state = RX_MARK;
            } else {
                rx_state = RX_IDLE;
            }
            if (bit_count == 32) {
                decoded_data = ir_data_buffer;
                data_ready = true;
                rx_state = RX_IDLE;
            }
        }
    } else {
        if (rx_state == RX_MARK) {
            if (delta_us >= 500 && delta_us < 2500) {
                rx_state = RX_SPACE;
            } else {
                rx_state = RX_IDLE;
            }
        }
    }
}

int ir_rx_init(const struct gpio_dt_spec *ir_pin, ir_rx_cb_t rx_cb)
{
    rx_ir_pin = ir_pin;
    app_rx_cb = rx_cb;

    if (!gpio_is_ready_dt(rx_ir_pin)) {
        return -1;
    }
    int ret = gpio_pin_configure_dt(rx_ir_pin, GPIO_INPUT);
    if (ret) {
        return ret;
    }
    ret = gpio_pin_interrupt_configure_dt(rx_ir_pin, GPIO_INT_EDGE_BOTH);
    if (ret) {
        return ret;
    }
    gpio_init_callback(&ir_cb_data, ir_triggered, BIT(rx_ir_pin->pin));
    gpio_add_callback(rx_ir_pin->port, &ir_cb_data);
    return 0;
}

void ir_rx_loop(void)
{
    unsigned int key = irq_lock();
    rx_state_t cur_state = rx_state;
    uint32_t cycles_since = k_cycle_get_32() - last_cycles;
    uint8_t cur_bits = bit_count;
    uint32_t cur_data = ir_data_buffer;
    irq_unlock(key);

    if (cur_state == RX_SPACE) {
        uint32_t delta_us = k_cyc_to_us_floor32(cycles_since);
        if (delta_us > 10000) {
            if (cur_bits == 31) {
                uint8_t b0 = cur_data & 0xFF;
                uint8_t b1 = (cur_data >> 8) & 0xFF;
                uint8_t b2 = (cur_data >> 16) & 0xFF;
                uint8_t partial = (cur_data >> 24) & 0x7F;
                uint8_t raw[3] = { b0, b1, b2 };
                uint8_t expected_crc = crc8_calc(raw, 3);
                if ((expected_crc & 0x7F) == partial) {
                    uint8_t missing_bit = (expected_crc >> 7) & 1;
                    cur_data |= ((uint32_t)missing_bit << 31);
                    decoded_data = cur_data;
                    data_ready = true;
                }
            }
            rx_state = RX_IDLE;
        }
    }

    if (data_ready) {
        uint8_t player_id = decoded_data & 0xFF;
        uint8_t weapon_id = (decoded_data >> 8) & 0xFF;
        uint8_t team_id   = (decoded_data >> 16) & 0xFF;
        uint8_t rx_crc    = (decoded_data >> 24) & 0xFF;

        uint8_t raw[3] = { player_id, weapon_id, team_id };
        bool valid = (rx_crc == crc8_calc(raw, 3));

        if (app_rx_cb) {
            ir_packet_t pkt = {
                .player_id = player_id,
                .weapon_id = weapon_id,
                .team_id   = team_id,
            };
            app_rx_cb(&pkt, valid);
        }
        data_ready = false;
    }
}
