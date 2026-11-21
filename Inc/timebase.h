/**
 * @file  timebase.h
 * @brief 1 ms SysTick time base.
 */
#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdint.h>

void     timebase_init(void);
uint32_t millis(void);
void     delay_ms(uint32_t ms);

#endif /* TIMEBASE_H */
