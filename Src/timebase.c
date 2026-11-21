/**
 * @file  timebase.c
 * @brief 1 ms SysTick time base.
 */
#include "timebase.h"
#include "board.h"
#include "stm32f103_regs.h"

static volatile uint32_t s_ticks_ms;

/* Overrides the weak alias in startup_stm32f103xx.S */
void SysTick_Handler(void)
{
    s_ticks_ms++;
}

void timebase_init(void)
{
    SYST_RVR = (CORE_CLOCK_HZ / 1000UL) - 1UL;
    SYST_CVR = 0U;
    SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

uint32_t millis(void)
{
    return s_ticks_ms;
}

/* Busy wait on purpose: no WFI sleep. While the core sleeps, ST-Link reads
 * of RAM made while the CPU is running came back as zeros on this board. */
void delay_ms(uint32_t ms)
{
    uint32_t start = s_ticks_ms;
    while ((s_ticks_ms - start) < ms) {
    }
}
