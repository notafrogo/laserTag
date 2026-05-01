#ifndef IR_HANDLER_H
#define IR_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

/* Data packet definition */
typedef struct {
    uint8_t user_id;
    uint8_t damage;
} ir_packet_t;

/* Callback when a hit is detected (valid or invalid checksum) */
typedef void (*ir_handler_rx_cb_t)(const ir_packet_t *packet, bool valid);

/**
 * @brief Initialize the IR transmitter
 * 
 * @param pwm_dev The PWM device used for the IR LED
 * @param carrier_period_us Period of the IR carrier in microseconds (e.g., 26 for ~38kHz)
 */
void ir_handler_tx_init(const struct device *pwm_dev, uint32_t carrier_period_us);

/**
 * @brief Transmit an IR packet
 * 
 * @param packet Pointer to the packet to send
 */
void ir_handler_tx_send(const ir_packet_t *packet);

/**
 * @brief Initialize the IR receiver
 * 
 * @param ir_pin The GPIO specification for the IR sensor
 * @param rx_cb Callback function to call when a packet passes or fails verification
 * @return 0 on success, negative error code on failure
 */
int ir_handler_rx_init(const struct gpio_dt_spec *ir_pin, ir_handler_rx_cb_t rx_cb);

/**
 * @brief Process the IR receiver timeout and hit events.
 *        This should be called periodically in a loop or thread (e.g. every 10ms).
 */
void ir_handler_rx_loop(void);

#endif /* IR_HANDLER_H */
