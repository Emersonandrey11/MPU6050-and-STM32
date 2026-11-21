/**
 * @file  stm32f103_regs.h
 * @brief Minimal register definitions for the STM32F103 (RM0008).
 *
 * The project has no CMSIS/HAL, so only the registers this firmware
 * uses are defined here.
 */
#ifndef STM32F103_REGS_H
#define STM32F103_REGS_H

#include <stdint.h>

#define REG32(addr)            (*(volatile uint32_t *)(addr))

/* ---- RCC ---------------------------------------------------------------- */
#define RCC_BASE               0x40021000UL
#define RCC_APB2RSTR           REG32(RCC_BASE + 0x0CUL)
#define RCC_APB1RSTR           REG32(RCC_BASE + 0x10UL)
#define RCC_APB2ENR            REG32(RCC_BASE + 0x18UL)
#define RCC_APB1ENR            REG32(RCC_BASE + 0x1CUL)

#define RCC_APB2ENR_AFIOEN     (1UL << 0)
#define RCC_APB2ENR_IOPBEN     (1UL << 3)
#define RCC_APB2ENR_IOPCEN     (1UL << 4)
#define RCC_APB1ENR_I2C1EN     (1UL << 21)
#define RCC_APB1RSTR_I2C1RST   (1UL << 21)

/* ---- GPIO --------------------------------------------------------------- */
#define GPIOB_BASE             0x40010C00UL
#define GPIOC_BASE             0x40011000UL

#define GPIO_CRL(port)         REG32((port) + 0x00UL)
#define GPIO_CRH(port)         REG32((port) + 0x04UL)
#define GPIO_IDR(port)         REG32((port) + 0x08UL)
#define GPIO_ODR(port)         REG32((port) + 0x0CUL)
#define GPIO_BSRR(port)        REG32((port) + 0x10UL)
#define GPIO_BRR(port)         REG32((port) + 0x14UL)

/* 4-bit pin configuration nibbles: CNF[1:0] MODE[1:0] */
#define GPIO_MODE_IN_FLOATING  0x4U   /* input, high impedance            */
#define GPIO_MODE_IN_PULL      0x8U   /* input, pull-up/down set by ODR   */
#define GPIO_MODE_OUT_PP_2MHZ  0x2U   /* general purpose push-pull        */
#define GPIO_MODE_OUT_OD_2MHZ  0x6U   /* general purpose open-drain       */
#define GPIO_MODE_AF_OD_2MHZ   0xEU   /* alternate function open-drain    */

static inline void gpio_config(uint32_t port, uint32_t pin, uint32_t mode)
{
    volatile uint32_t *cr = (pin < 8U) ? &GPIO_CRL(port) : &GPIO_CRH(port);
    uint32_t shift = (pin & 7U) * 4U;
    *cr = (*cr & ~(0xFUL << shift)) | (mode << shift);
}

/* ---- I2C1 --------------------------------------------------------------- */
#define I2C1_BASE              0x40005400UL
#define I2C1_CR1               REG32(I2C1_BASE + 0x00UL)
#define I2C1_CR2               REG32(I2C1_BASE + 0x04UL)
#define I2C1_DR                REG32(I2C1_BASE + 0x10UL)
#define I2C1_SR1               REG32(I2C1_BASE + 0x14UL)
#define I2C1_SR2               REG32(I2C1_BASE + 0x18UL)
#define I2C1_CCR               REG32(I2C1_BASE + 0x1CUL)
#define I2C1_TRISE             REG32(I2C1_BASE + 0x20UL)

#define I2C_CR1_PE             (1UL << 0)
#define I2C_CR1_START          (1UL << 8)
#define I2C_CR1_STOP           (1UL << 9)
#define I2C_CR1_ACK            (1UL << 10)
#define I2C_CR1_POS            (1UL << 11)
#define I2C_CR1_SWRST          (1UL << 15)

#define I2C_SR1_SB             (1UL << 0)
#define I2C_SR1_ADDR           (1UL << 1)
#define I2C_SR1_BTF            (1UL << 2)
#define I2C_SR1_RXNE           (1UL << 6)
#define I2C_SR1_TXE            (1UL << 7)
#define I2C_SR1_BERR           (1UL << 8)
#define I2C_SR1_ARLO           (1UL << 9)
#define I2C_SR1_AF             (1UL << 10)

#define I2C_SR2_BUSY           (1UL << 1)

/* ---- SysTick (Cortex-M3 core) ------------------------------------------- */
#define SYST_CSR               REG32(0xE000E010UL)
#define SYST_RVR               REG32(0xE000E014UL)
#define SYST_CVR               REG32(0xE000E018UL)
#define SYST_CSR_ENABLE        (1UL << 0)
#define SYST_CSR_TICKINT       (1UL << 1)
#define SYST_CSR_CLKSOURCE     (1UL << 2)

/* ---- DBGMCU ------------------------------------------------------------- */
#define DBGMCU_CR              REG32(0xE0042004UL)
#define DBGMCU_CR_DBG_SLEEP    (1UL << 0)   /* keep debug access alive in WFI */

/* ---- Interrupt masking -------------------------------------------------- */
static inline uint32_t irq_save(void)
{
    uint32_t primask;
    __asm volatile ("mrs %0, primask\n\tcpsid i" : "=r" (primask) :: "memory");
    return primask;
}

static inline void irq_restore(uint32_t primask)
{
    __asm volatile ("msr primask, %0" :: "r" (primask) : "memory");
}

#endif /* STM32F103_REGS_H */
