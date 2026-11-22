/**
 * @file  clock.h
 * @brief 48 MHz system clock from the 8 MHz crystal (needed by USB).
 */
#ifndef CLOCK_H
#define CLOCK_H

/* Called by the startup code before main(); defined in clock.c. */
void SystemInit(void);

/* 1 if SYSCLK runs from the crystal-based PLL (USB clock in tolerance),
 * 0 if the crystal did not start and the HSI fallback is used. */
int clock_uses_crystal(void);

#endif /* CLOCK_H */
