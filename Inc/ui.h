/**
 * @file  ui.h
 * @brief MPU6050 dashboard on the 240x320 TFT.
 *
 * The UI reads everything from g_telemetry (the same data the PC sees), so it
 * needs no hooks into the sensor code. ui_task() does at most a few small
 * drawing jobs per call (well under 1 ms each at 12 MHz SPI), so the 100 Hz
 * sensor loop and the PC link keep their timing.
 */
#ifndef UI_H
#define UI_H

#include <stdint.h>

void ui_init(void);              /* blocking: panel reset + static layout */
void ui_task(uint32_t now_ms);   /* call on every pass of the main loop   */

#endif /* UI_H */
