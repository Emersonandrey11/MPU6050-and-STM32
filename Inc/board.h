/**
 * @file  board.h
 * @brief Board wiring (see HARDWARE.md) and clock settings.
 */
#ifndef BOARD_H
#define BOARD_H

/* Clocks (clock.c): 8 MHz crystal x 6 = 48 MHz, because USB needs an exact
 * 48 MHz clock. APB1 is divided by 2 (24 MHz); APB1 timers get x2 back. */
#define CORE_CLOCK_HZ   48000000UL
#define PCLK1_HZ        24000000UL    /* I2C1, TIM3 registers, USB registers */
#define PCLK2_HZ        48000000UL    /* GPIO, SPI1                          */
#define TIM_APB1_HZ     48000000UL    /* APB1 prescaler != 1 -> timers at 2 x PCLK1 */

/* Native USB port (micro-USB on the Blue Pill). The 1.5 kOhm D+ pull-up
 * (R10) is fixed to 3.3 V, so firmware drives PA12 low for a moment to make
 * the PC see a disconnect/reconnect after every reset. */
#define USB_DM_PIN      11U   /* PA11 <-> D- */
#define USB_DP_PIN      12U   /* PA12 <-> D+ */

/* On-board LED: PC13 (active-low on the Blue Pill) */
#define LED_PIN         13U

/* MPU6050 wiring, all on GPIOB */
#define MPU_SCL_PIN     6U    /* PB6  -> SCL  (I2C1 SCL, no remap)            */
#define MPU_SDA_PIN     7U    /* PB7  -> SDA  (I2C1 SDA, no remap)            */
#define MPU_XCL_PIN     10U   /* PB10 -> XCL  (MPU auxiliary bus, unused)     */
#define MPU_XDA_PIN     11U   /* PB11 -> XDA  (MPU auxiliary bus, unused)     */
#define MPU_INT_PIN     12U   /* PB12 <- INT  (data ready / motion)           */
#define MPU_AD0_PIN     13U   /* PB13 -> AD0  (address select: 0x68 / 0x69)   */

/* 2.8" 240x320 ST7789V TFT on SPI1 (no remap). Module pin -> Blue Pill pin. */
#define LCD_SCK_PIN     5U    /* PA5  -> SCK   SPI1_SCK,  AF push-pull         */
#define LCD_MISO_PIN    6U    /* PA6  <- SDO   SPI1_MISO, input (panel ID only) */
#define LCD_MOSI_PIN    7U    /* PA7  -> SDI   SPI1_MOSI, AF push-pull         */
#define LCD_CS_PIN      4U    /* PA4  -> CS    GPIO, active low                */
#define LCD_DC_PIN      3U    /* PA3  -> DC    GPIO, low = command, high = data */
#define LCD_RST_PIN     2U    /* PA2  -> RESET GPIO, active low                */
#define LCD_BL_PIN      0U    /* PB0  -> LED   TIM3_CH3 PWM, high = light on    */

/* Panel options. Change these if the picture looks wrong:
 *   colours inverted (black background shows white) -> flip LCD_INVERT
 *   red and blue swapped                            -> LCD_MADCTL ^= 0x08
 *   picture upside down                             -> LCD_MADCTL ^= 0xC0 */
#define LCD_INVERT      0U    /* this panel shows true colours with INVOFF   */
#define LCD_MADCTL      0x00U /* portrait, RGB order, top-left origin         */

/* Backlight brightness in percent (PWM duty). The backlight is the largest
 * load on the 3.3 V rail (~50-80 mA at 100 %); the current scales with it. */
#define LCD_BRIGHTNESS  50U

#endif /* BOARD_H */
