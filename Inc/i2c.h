/**
 * @file  i2c.h
 * @brief Polling I2C1 master driver (PB6 = SCL, PB7 = SDA), 100 kHz.
 *
 * All functions return 0 on success and -1 on error (NACK, bus error or
 * timeout). After an error the bus is released and, if it is stuck,
 * recovered automatically.
 */
#ifndef I2C_H
#define I2C_H

#include <stdint.h>

void     i2c_init(void);
int      i2c_probe(uint8_t addr7);
int      i2c_write(uint8_t addr7, uint8_t reg, const uint8_t *data, uint32_t len);
int      i2c_read(uint8_t addr7, uint8_t reg, uint8_t *data, uint32_t len);
uint32_t i2c_error_count(void);

#endif /* I2C_H */
