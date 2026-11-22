# HW configurations

## Native USB (micro-USB connector)

- PA11: USB D-
- PA12: USB D+ (R10 pull-up to 3.3 V must be 1.5 kOhm)
- PB2: BOOT1 jumper, read by the DFU bootloader (1 = stay in DFU mode)
- VBUS 5 V: powers the board through the on-board regulator
- Clock: 8 MHz crystal (HSE) x 6 = 48 MHz, required by USB

## MPU6050

- PB12: INT
- PB13: AD0
- PB7: SDA
- PB6: scl
- PB10: xcl
- PB11: xda

## 2.8" ST7789V TFT (240x320, SPI)

Schematic: [docs/wiring-tft.svg](docs/wiring-tft.svg)

- PA5: SCK (SPI1_SCK)
- PA7: SDI / MOSI (SPI1_MOSI)
- PA6: SDO / MISO (SPI1_MISO, optional: panel ID read only)
- PA4: CS
- PA3: DC
- PA2: RESET
- PB0: LED (backlight, TIM3_CH3 PWM at 20 kHz)
- VCC: 3.3 V
- GND: GND
