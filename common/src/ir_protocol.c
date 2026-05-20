#include "ir_protocol.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <string.h>

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

static void tx_raw(const uint8_t *bytes, size_t len)
{
    if (!tx_pwm) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        for (int j = 0; j < 8; j++) {
            bool bit = (bytes[i] >> j) & 1;
            pwm_set_cycles(tx_pwm, 0, tx_carrier_period, tx_carrier_period / 2, 0);
            k_msleep(1);
            pwm_set_cycles(tx_pwm, 0, tx_carrier_period, 0, 0);
            k_msleep(bit ? 3 : 1);
        }
    }
    pwm_set_cycles(tx_pwm, 0, tx_carrier_period, 0, 0);
}

void ir_tx_send(const ir_packet_t *packet)
{
    uint8_t raw[3] = { packet->player_id, packet->weapon_id, packet->team_id };
    uint8_t buffer[IR_HIT_PACKET_LEN] = { raw[0], raw[1], raw[2], crc8_calc(raw, 3) };
    tx_raw(buffer, sizeof(buffer));
}

void ir_tx_send_pairing(const ir_pairing_packet_t *packet)
{
    uint8_t buffer[IR_PAIRING_PACKET_LEN];
    buffer[0] = IR_PAIRING_PREAMBLE;
    buffer[1] = (uint8_t)(packet->nonce & 0xFF);
    buffer[2] = (uint8_t)((packet->nonce >> 8) & 0xFF);
    buffer[3] = packet->player_id;
    buffer[4] = crc8_calc(buffer, 4);
    tx_raw(buffer, sizeof(buffer));
}

/* ===== RX =====
 *
 * The ISR decodes bits into rx_bytes until a long silence (handled in
 * ir_rx_loop) indicates end-of-frame. The polling loop then dispatches
 * based on byte count: 4 bytes = hit, 5 bytes with the pairing preamble
 * = pairing. Anything else is dropped.
 */

#define IR_RX_BUF_MAX  IR_PAIRING_PACKET_LEN

typedef enum { RX_IDLE, RX_MARK, RX_SPACE } rx_state_t;

static volatile rx_state_t rx_state = RX_IDLE;
static volatile uint32_t   last_cycles;
static volatile uint8_t    bit_count;
static volatile uint8_t    rx_bytes[IR_RX_BUF_MAX];

/* Snapshot consumed by ir_rx_loop */
static volatile bool       data_ready;
static volatile uint8_t    decoded_bytes[IR_RX_BUF_MAX];
static volatile uint8_t    decoded_len;

static ir_rx_cb_t          app_rx_cb;
static ir_pairing_rx_cb_t  app_pairing_cb;
static struct gpio_callback ir_cb_data;
static const struct gpio_dt_spec *rx_ir_pin;

static inline void rx_store_bit(uint8_t value)
{
    if (bit_count >= IR_RX_BUF_MAX * 8) {
        return;
    }
    if (value) {
        rx_bytes[bit_count / 8] |= (uint8_t)(1u << (bit_count % 8));
    }
    bit_count++;
}

static void rx_reset(void)
{
    rx_state = RX_IDLE;
    bit_count = 0;
    memset((void *)rx_bytes, 0, sizeof(rx_bytes));
}

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
            memset((void *)rx_bytes, 0, sizeof(rx_bytes));
        } else if (rx_state == RX_SPACE) {
            if (delta_us >= 500 && delta_us < 2200) {
                rx_store_bit(0);
                rx_state = RX_MARK;
            } else if (delta_us >= 2200 && delta_us < 5000) {
                rx_store_bit(1);
                rx_state = RX_MARK;
            } else {
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

void ir_rx_set_pairing_cb(ir_pairing_rx_cb_t cb)
{
    app_pairing_cb = cb;
}

void ir_rx_loop(void)
{
    /* Detect end-of-frame via long silence on the wire and snapshot the
     * decoded bytes for dispatch outside the ISR.
     */
    unsigned int key = irq_lock();
    rx_state_t cur_state = rx_state;
    uint32_t cycles_since = k_cycle_get_32() - last_cycles;
    uint8_t cur_bits = bit_count;

    if (cur_state == RX_SPACE) {
        uint32_t delta_us = k_cyc_to_us_floor32(cycles_since);
        if (delta_us > 10000) {
            if ((cur_bits % 8) == 0 && cur_bits > 0 && !data_ready) {
                decoded_len = cur_bits / 8;
                memcpy((void *)decoded_bytes, (const void *)rx_bytes, decoded_len);
                data_ready = true;
            }
            rx_reset();
        }
    }
    irq_unlock(key);

    if (!data_ready) {
        return;
    }

    uint8_t buf[IR_RX_BUF_MAX];
    uint8_t len = decoded_len;
    memcpy(buf, (const void *)decoded_bytes, len);
    data_ready = false;

    if (len == IR_HIT_PACKET_LEN) {
        bool valid = (buf[3] == crc8_calc(buf, 3));
        if (app_rx_cb) {
            ir_packet_t pkt = {
                .player_id = buf[0],
                .weapon_id = buf[1],
                .team_id   = buf[2],
            };
            app_rx_cb(&pkt, valid);
        }
    } else if (len == IR_PAIRING_PACKET_LEN && buf[0] == IR_PAIRING_PREAMBLE) {
        bool valid = (buf[4] == crc8_calc(buf, 4));
        if (valid && app_pairing_cb) {
            ir_pairing_packet_t pkt = {
                .nonce     = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8),
                .player_id = buf[3],
            };
            app_pairing_cb(&pkt);
        }
    }
    /* Other lengths or bad preamble: silently drop. */
}
