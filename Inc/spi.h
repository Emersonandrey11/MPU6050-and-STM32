/**
 * @file  spi.h
 * @brief Polling SPI1 master driver (PA5 = SCK, PA6 = MISO, PA7 = MOSI).
 *
 * Mode 0 (CPOL = 0, CPHA = 0), MSB first, fPCLK2 / 4 = 12 MHz (48 MHz clock).
 * Chip select is a plain GPIO handled by the device driver.
 * Functions that wait return 0 on success and -1 on timeout.
 */
#ifndef SPI_H
#define SPI_H

#include <stdint.h>

void     spi1_init(void);
int      spi1_frame16(int on);                 /* 16-bit (1) or 8-bit (0) frames */
int      spi1_send(uint16_t frame);            /* queue one frame, do not wait   */
int      spi1_flush(void);                     /* wait until the last bit is out */
int      spi1_transfer(uint8_t tx, uint8_t *rx);
uint32_t spi1_error_count(void);

#endif /* SPI_H */
