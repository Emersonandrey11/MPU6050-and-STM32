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
#define RCC_CR                 REG32(RCC_BASE + 0x00UL)
#define RCC_CFGR               REG32(RCC_BASE + 0x04UL)
#define RCC_APB2RSTR           REG32(RCC_BASE + 0x0CUL)
#define RCC_APB1RSTR           REG32(RCC_BASE + 0x10UL)
#define RCC_APB2ENR            REG32(RCC_BASE + 0x18UL)
#define RCC_APB1ENR            REG32(RCC_BASE + 0x1CUL)

#define RCC_CR_HSEON           (1UL << 16)
#define RCC_CR_HSERDY          (1UL << 17)
#define RCC_CR_PLLON           (1UL << 24)
#define RCC_CR_PLLRDY          (1UL << 25)

#define RCC_CFGR_SW_MASK       (3UL << 0)
#define RCC_CFGR_SW_HSI        (0UL << 0)
#define RCC_CFGR_SW_PLL        (2UL << 0)
#define RCC_CFGR_SWS_MASK      (3UL << 2)
#define RCC_CFGR_SWS_HSI       (0UL << 2)
#define RCC_CFGR_SWS_PLL       (2UL << 2)
#define RCC_CFGR_HPRE_DIV1     (0UL << 4)
#define RCC_CFGR_PPRE1_DIV2    (4UL << 8)    /* APB1 = HCLK / 2 */
#define RCC_CFGR_PPRE2_DIV1    (0UL << 11)   /* APB2 = HCLK     */
#define RCC_CFGR_PLLSRC_HSE    (1UL << 16)   /* 0 = HSI / 2     */
#define RCC_CFGR_PLLMUL(x)     ((uint32_t)((x) - 2U) << 18)   /* x2..x16 */
#define RCC_CFGR_USBPRE_DIV1   (1UL << 22)   /* USBCLK = PLLCLK (0 = PLLCLK / 1.5) */

#define RCC_APB2ENR_AFIOEN     (1UL << 0)
#define RCC_APB2ENR_IOPAEN     (1UL << 2)
#define RCC_APB2ENR_IOPBEN     (1UL << 3)
#define RCC_APB2ENR_IOPCEN     (1UL << 4)
#define RCC_APB2ENR_SPI1EN     (1UL << 12)
#define RCC_APB2RSTR_SPI1RST   (1UL << 12)
#define RCC_APB1ENR_TIM3EN     (1UL << 1)
#define RCC_APB1ENR_I2C1EN     (1UL << 21)
#define RCC_APB1ENR_USBEN      (1UL << 23)
#define RCC_APB1ENR_BKPEN      (1UL << 27)
#define RCC_APB1ENR_PWREN      (1UL << 28)
#define RCC_APB1RSTR_I2C1RST   (1UL << 21)
#define RCC_APB1RSTR_USBRST    (1UL << 23)

/* ---- Flash interface (FPEC) -------------------------------------------- */
#define FLASH_BASE_R           0x40022000UL
#define FLASH_ACR              REG32(FLASH_BASE_R + 0x00UL)
#define FLASH_KEYR             REG32(FLASH_BASE_R + 0x04UL)
#define FLASH_SR               REG32(FLASH_BASE_R + 0x0CUL)
#define FLASH_CR               REG32(FLASH_BASE_R + 0x10UL)
#define FLASH_AR               REG32(FLASH_BASE_R + 0x14UL)

#define FLASH_ACR_LATENCY_1WS  (1UL << 0)    /* 24 < SYSCLK <= 48 MHz */
#define FLASH_ACR_PRFTBE       (1UL << 4)
#define FLASH_KEY1             0x45670123UL
#define FLASH_KEY2             0xCDEF89ABUL
#define FLASH_SR_BSY           (1UL << 0)
#define FLASH_SR_PGERR         (1UL << 2)
#define FLASH_SR_WRPRTERR      (1UL << 4)
#define FLASH_SR_EOP           (1UL << 5)
#define FLASH_CR_PG            (1UL << 0)
#define FLASH_CR_PER           (1UL << 1)
#define FLASH_CR_STRT          (1UL << 6)
#define FLASH_CR_LOCK          (1UL << 7)

/* Electronic signature (system memory) */
#define FLASH_SIZE_KB          (*(volatile const uint16_t *)0x1FFFF7E0UL)
#define UNIQUE_ID_BASE         0x1FFFF7E8UL  /* 96-bit unique device ID */

/* ---- Backup registers + power control ----------------------------------- */
#define PWR_CR                 REG32(0x40007000UL)
#define PWR_CR_DBP             (1UL << 8)    /* allow writes to the backup domain */
#define BKP_DR1                REG32(0x40006C04UL)   /* 16 bits, survive system resets */

/* ---- USB full-speed device (RM0008 "USB full-speed device interface") --- */
#define USB_BASE               0x40005C00UL
#define USB_EPR(n)             REG32(USB_BASE + 4UL * (n))
#define USB_CNTR               REG32(USB_BASE + 0x40UL)
#define USB_ISTR               REG32(USB_BASE + 0x44UL)
#define USB_FNR                REG32(USB_BASE + 0x48UL)
#define USB_DADDR              REG32(USB_BASE + 0x4CUL)
#define USB_BTABLE             REG32(USB_BASE + 0x50UL)
#define USB_PMA_BASE           0x40006000UL  /* 512 bytes, 16-bit words at a 32-bit stride */

#define USB_CNTR_FRES          (1UL << 0)
#define USB_CNTR_PDWN          (1UL << 1)
#define USB_CNTR_RESETM        (1UL << 10)
#define USB_CNTR_CTRM          (1UL << 15)

#define USB_ISTR_EP_ID         (0xFUL << 0)
#define USB_ISTR_RESET         (1UL << 10)
#define USB_ISTR_CTR           (1UL << 15)

#define USB_DADDR_EF           (1UL << 7)

/* EPnR bits. Access types matter: CTR_RX/CTR_TX are cleared by writing 0
 * (writing 1 leaves them), DTOG_x and STAT_x TOGGLE when written with 1,
 * SETUP is read-only, EA/EP_TYPE/EP_KIND are plain read/write. */
#define USB_EP_EA              (0xFUL << 0)
#define USB_EP_STAT_TX         (3UL << 4)
#define USB_EP_DTOG_TX         (1UL << 6)
#define USB_EP_CTR_TX          (1UL << 7)
#define USB_EP_KIND            (1UL << 8)
#define USB_EP_TYPE            (3UL << 9)
#define USB_EP_SETUP           (1UL << 11)
#define USB_EP_STAT_RX         (3UL << 12)
#define USB_EP_DTOG_RX         (1UL << 14)
#define USB_EP_CTR_RX          (1UL << 15)

#define USB_EP_TYPE_BULK       (0UL << 9)
#define USB_EP_TYPE_CONTROL    (1UL << 9)
#define USB_EP_TYPE_ISO        (2UL << 9)
#define USB_EP_TYPE_INTERRUPT  (3UL << 9)

#define USB_STAT_DISABLED      0U
#define USB_STAT_STALL         1U
#define USB_STAT_NAK           2U
#define USB_STAT_VALID         3U

/* ---- GPIO --------------------------------------------------------------- */
#define GPIOA_BASE             0x40010800UL
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
#define GPIO_MODE_OUT_PP_50MHZ 0x3U   /* push-pull, fast edges (SPI side)  */
#define GPIO_MODE_OUT_OD_2MHZ  0x6U   /* general purpose open-drain       */
#define GPIO_MODE_AF_OD_2MHZ   0xEU   /* alternate function open-drain    */
#define GPIO_MODE_AF_PP_2MHZ   0xAU   /* alternate function push-pull, slow edges */
#define GPIO_MODE_AF_PP_50MHZ  0xBU   /* alternate function push-pull     */

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

/* ---- SPI1 (APB2, RM0008 section 25) ------------------------------------ */
#define SPI1_BASE              0x40013000UL
#define SPI1_CR1               REG32(SPI1_BASE + 0x00UL)
#define SPI1_CR2               REG32(SPI1_BASE + 0x04UL)
#define SPI1_SR                REG32(SPI1_BASE + 0x08UL)
#define SPI1_DR                REG32(SPI1_BASE + 0x0CUL)

#define SPI_CR1_CPHA           (1UL << 0)
#define SPI_CR1_CPOL           (1UL << 1)
#define SPI_CR1_MSTR           (1UL << 2)
#define SPI_CR1_BR_DIV2        (0UL << 3)   /* BR[2:0] = 000 -> fPCLK / 2 */
#define SPI_CR1_BR_DIV4        (1UL << 3)   /* BR[2:0] = 001 -> fPCLK / 4 */
#define SPI_CR1_SPE            (1UL << 6)
#define SPI_CR1_SSI            (1UL << 8)
#define SPI_CR1_SSM            (1UL << 9)
#define SPI_CR1_DFF            (1UL << 11)  /* 1 = 16-bit frames */

#define SPI_SR_RXNE            (1UL << 0)
#define SPI_SR_TXE             (1UL << 1)
#define SPI_SR_BSY             (1UL << 7)

/* ---- TIM3 (APB1, general-purpose timer) -------------------------------- */
#define TIM3_BASE              0x40000400UL
#define TIM3_CR1               REG32(TIM3_BASE + 0x00UL)
#define TIM3_EGR               REG32(TIM3_BASE + 0x14UL)
#define TIM3_CCMR2             REG32(TIM3_BASE + 0x1CUL)
#define TIM3_CCER              REG32(TIM3_BASE + 0x20UL)
#define TIM3_PSC               REG32(TIM3_BASE + 0x28UL)
#define TIM3_ARR               REG32(TIM3_BASE + 0x2CUL)
#define TIM3_CCR3              REG32(TIM3_BASE + 0x3CUL)

#define TIM_CR1_CEN            (1UL << 0)
#define TIM_CR1_ARPE           (1UL << 7)
#define TIM_EGR_UG             (1UL << 0)
#define TIM_CCMR2_OC3PE        (1UL << 3)
#define TIM_CCMR2_OC3M_PWM1    (6UL << 4)   /* OC3M = 110: high while CNT < CCR3 */
#define TIM_CCER_CC3E          (1UL << 8)

/* ---- SysTick (Cortex-M3 core) ------------------------------------------- */
#define SYST_CSR               REG32(0xE000E010UL)
#define SYST_RVR               REG32(0xE000E014UL)
#define SYST_CVR               REG32(0xE000E018UL)
#define SYST_CSR_ENABLE        (1UL << 0)
#define SYST_CSR_TICKINT       (1UL << 1)
#define SYST_CSR_CLKSOURCE     (1UL << 2)

/* ---- SCB / NVIC (Cortex-M3 core, PM0056) -------------------------------- */
#define SCB_VTOR               REG32(0xE000ED08UL)
#define SCB_AIRCR              REG32(0xE000ED0CUL)
#define SCB_AIRCR_SYSRESET     ((0x05FAUL << 16) | (1UL << 2))   /* VECTKEY + SYSRESETREQ */
#define NVIC_ISER(n)           REG32(0xE000E100UL + 4UL * (n))
#define NVIC_ICER(n)           REG32(0xE000E180UL + 4UL * (n))
#define USB_LP_CAN_RX0_IRQn    20U

/* Start another image: load its initial stack pointer and jump to its reset
 * handler (the bootloader uses this to start the application). */
static inline void start_image(uint32_t sp, uint32_t pc)
{
    __asm volatile ("msr msp, %0\n\tbx %1" :: "r" (sp), "r" (pc) : "memory");
    for (;;) {
    }
}

static inline void system_reset(void)
{
    __asm volatile ("dsb" ::: "memory");
    SCB_AIRCR = SCB_AIRCR_SYSRESET;
    __asm volatile ("dsb" ::: "memory");
    for (;;) {
    }
}

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
