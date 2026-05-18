#ifndef IR_PROTOCOL_H
#define IR_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

/* IR Packet: 4 bytes on the wire (player_id + weapon_id + team_id + CRC-8) */
typedef struct {
    uint8_t player_id;
    uint8_t weapon_id;
    uint8_t team_id;
} ir_packet_t;

/* CRC-8 (polynomial 0x07, init 0x00, no final XOR) */
uint8_t crc8_calc(const uint8_t *data, size_t len);

/* TX */
void ir_tx_init(const struct device *pwm_dev, uint32_t carrier_period_us);
void ir_tx_send(const ir_packet_t *packet);

/* RX callback */
typedef void (*ir_rx_cb_t)(const ir_packet_t *packet, bool crc_valid);

int  ir_rx_init(const struct gpio_dt_spec *ir_pin, ir_rx_cb_t rx_cb);
void ir_rx_loop(void);

#endif /* IR_PROTOCOL_H */
