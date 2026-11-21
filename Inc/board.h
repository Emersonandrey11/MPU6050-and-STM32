/**
 * @file  board.h
 * @brief Board wiring (see HARDWARE.md) and clock settings.
 */
#ifndef BOARD_H
#define BOARD_H

/* Clocks: the chip runs from the internal 8 MHz HSI oscillator (no PLL). */
#define CORE_CLOCK_HZ   8000000UL
#define PCLK1_HZ        8000000UL

/* On-board LED: PC13 (active-low on the Blue Pill) */
#define LED_PIN         13U

/* MPU6050 wiring, all on GPIOB */
#define MPU_SCL_PIN     6U    /* PB6  -> SCL  (I2C1 SCL, no remap)            */
#define MPU_SDA_PIN     7U    /* PB7  -> SDA  (I2C1 SDA, no remap)            */
#define MPU_XCL_PIN     10U   /* PB10 -> XCL  (MPU auxiliary bus, unused)     */
#define MPU_XDA_PIN     11U   /* PB11 -> XDA  (MPU auxiliary bus, unused)     */
#define MPU_INT_PIN     12U   /* PB12 <- INT  (data ready / motion)           */
#define MPU_AD0_PIN     13U   /* PB13 -> AD0  (address select: 0x68 / 0x69)   */

#endif /* BOARD_H */
