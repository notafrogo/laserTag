#ifndef IR_PROTOCOL_H
#define IR_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#define IR_HIT_PACKET_LEN      4  /* player_id + weapon_id + team_id + CRC */
#define IR_PAIRING_PACKET_LEN  5  /* preamble + nonce[2] + player_id + CRC */
#define IR_PAIRING_PREAMBLE    0xFE

/* Hit packet: 4 bytes on the wire (player_id + weapon_id + team_id + CRC-8) */
typedef struct {
    uint8_t player_id;
    uint8_t weapon_id;
    uint8_t team_id;
} ir_packet_t;

/* Pairing packet: 5 bytes on the wire
 * (IR_PAIRING_PREAMBLE + addr_lsbs[2] LE + player_id + CRC-8)
 *
 * addr_lsbs carries the emitter's own BLE address bytes [0] and [1]
 * (the same two bytes the LT-E-XXXX name suffix encodes). The vest
 * verifies the connecting emitter's BLE address against this value at
 * connected() time — no GATT round-trip needed to bind pairing to the
 * physically-aimed emitter.
 */
typedef struct {
    uint16_t addr_lsbs;
    uint8_t  player_id;
} ir_pairing_packet_t;

/* CRC-8 (polynomial 0x07, init 0x00, no final XOR) */
uint8_t crc8_calc(const uint8_t *data, size_t len);

/* TX */
void ir_tx_init(const struct device *pwm_dev, uint32_t carrier_period_us);
void ir_tx_send(const ir_packet_t *packet);
void ir_tx_send_pairing(const ir_pairing_packet_t *packet);

/* RX callbacks */
typedef void (*ir_rx_cb_t)(const ir_packet_t *packet, bool crc_valid);
typedef void (*ir_pairing_rx_cb_t)(const ir_pairing_packet_t *packet);

int  ir_rx_init(const struct gpio_dt_spec *ir_pin, ir_rx_cb_t rx_cb);
void ir_rx_set_pairing_cb(ir_pairing_rx_cb_t cb);
void ir_rx_loop(void);

#endif /* IR_PROTOCOL_H */
