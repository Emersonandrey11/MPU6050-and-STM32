/**
 * @file  i2c.c
 * @brief Polling I2C1 master driver for the STM32F103 (RM0008 ch. 26).
 *
 * The receive sequences for 1, 2 and >2 bytes follow ST application note
 * AN2824; the short critical sections with interrupts masked are required
 * by that note (and by the F1 I2C errata) so that STOP/NACK are set in time.
 */
#include "i2c.h"
#include "board.h"
#include "stm32f103_regs.h"

#define I2C_SPEED_HZ   100000UL   /* standard mode */
/* Loop counts scale with the CPU clock so they mean the same time at any
 * speed (they were tuned at 8 MHz). */
#define CLOCK_SCALE    (CORE_CLOCK_HZ / 8000000UL)
#define I2C_TIMEOUT    (20000UL * CLOCK_SCALE)   /* polling iterations, roughly 10-20 ms */

static uint32_t s_errors;

/* ---- Pin handling and bus recovery -------------------------------------- */

static void bus_delay(void)
{
    for (volatile uint32_t i = 0; i < 10UL * CLOCK_SCALE; i++) {   /* ~5 us: <= 100 kHz bit-banging */
    }
}

static int sda_is_high(void)
{
    return (int)((GPIO_IDR(GPIOB_BASE) >> MPU_SDA_PIN) & 1UL);
}

/* If a slave was interrupted mid-byte (e.g. MCU reset during a read) it can
 * hold SDA low forever. Clock SCL by hand until it lets go, then send STOP. */
static void bus_recover(void)
{
    const uint32_t scl = 1UL << MPU_SCL_PIN;
    const uint32_t sda = 1UL << MPU_SDA_PIN;

    GPIO_BSRR(GPIOB_BASE) = scl | sda;
    gpio_config(GPIOB_BASE, MPU_SCL_PIN, GPIO_MODE_OUT_OD_2MHZ);
    gpio_config(GPIOB_BASE, MPU_SDA_PIN, GPIO_MODE_OUT_OD_2MHZ);
    bus_delay();

    for (uint32_t i = 0; (i < 9U) && !sda_is_high(); i++) {
        GPIO_BRR(GPIOB_BASE) = scl;
        bus_delay();
        GPIO_BSRR(GPIOB_BASE) = scl;
        bus_delay();
    }

    /* STOP condition: SDA rises while SCL is high */
    GPIO_BRR(GPIOB_BASE) = scl;
    bus_delay();
    GPIO_BRR(GPIOB_BASE) = sda;
    bus_delay();
    GPIO_BSRR(GPIOB_BASE) = scl;
    bus_delay();
    GPIO_BSRR(GPIOB_BASE) = sda;
    bus_delay();
}

void i2c_init(void)
{
    RCC_APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;
    RCC_APB1ENR |= RCC_APB1ENR_I2C1EN;

    I2C1_CR1 = 0U;
    bus_recover();
    gpio_config(GPIOB_BASE, MPU_SCL_PIN, GPIO_MODE_AF_OD_2MHZ);
    gpio_config(GPIOB_BASE, MPU_SDA_PIN, GPIO_MODE_AF_OD_2MHZ);

    /* Full peripheral reset: also clears a stuck BUSY flag (F1 errata) */
    RCC_APB1RSTR |= RCC_APB1RSTR_I2C1RST;
    RCC_APB1RSTR &= ~RCC_APB1RSTR_I2C1RST;
    I2C1_CR1 = I2C_CR1_SWRST;
    I2C1_CR1 = 0U;

    I2C1_CR2   = PCLK1_HZ / 1000000UL;              /* APB1 clock in MHz      */
    I2C1_CCR   = PCLK1_HZ / (2UL * I2C_SPEED_HZ);   /* 24 MHz: 120 -> 100 kHz */
    I2C1_TRISE = (PCLK1_HZ / 1000000UL) + 1UL;      /* 25: 1000 ns rise time  */
    I2C1_CR1   = I2C_CR1_PE;
}

uint32_t i2c_error_count(void)
{
    return s_errors;
}

/* ---- Low-level helpers -------------------------------------------------- */

static int wait_flag(uint32_t flag)
{
    for (uint32_t t = I2C_TIMEOUT; t != 0U; t--) {
        uint32_t sr1 = I2C1_SR1;
        if (sr1 & flag) {
            return 0;
        }
        if (sr1 & (I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO)) {
            return -1;
        }
    }
    return -1;
}

static int wait_stop_sent(void)
{
    for (uint32_t t = I2C_TIMEOUT; t != 0U; t--) {
        if ((I2C1_CR1 & I2C_CR1_STOP) == 0U) {
            return 0;
        }
    }
    return -1;
}

static int wait_bus_free(void)
{
    for (uint32_t t = I2C_TIMEOUT; t != 0U; t--) {
        if ((I2C1_SR2 & I2C_SR2_BUSY) == 0U) {
            return 0;
        }
    }
    return -1;
}

static void clear_addr(void)
{
    (void)I2C1_SR1;
    (void)I2C1_SR2;
}

/* Abort the transfer, release the bus and recover if needed. */
static int fail(void)
{
    s_errors++;
    I2C1_CR1 |= I2C_CR1_STOP;
    I2C1_SR1 = 0U;                                  /* clear AF/BERR/ARLO */
    I2C1_CR1 &= ~(I2C_CR1_POS | I2C_CR1_ACK);
    if ((wait_stop_sent() != 0) || (wait_bus_free() != 0)) {
        i2c_init();
    }
    return -1;
}

static int ensure_bus_free(void)
{
    if (wait_bus_free() == 0) {
        return 0;
    }
    i2c_init();
    return wait_bus_free();
}

static int send_address(uint8_t addr8)
{
    I2C1_CR1 |= I2C_CR1_START;
    if (wait_flag(I2C_SR1_SB) != 0) {
        return -1;
    }
    I2C1_DR = addr8;
    return wait_flag(I2C_SR1_ADDR);
}

/* START + address(write) + register pointer */
static int send_register(uint8_t addr7, uint8_t reg)
{
    if (ensure_bus_free() != 0) {
        return -1;
    }
    if (send_address((uint8_t)(addr7 << 1)) != 0) {
        return -1;
    }
    clear_addr();
    if (wait_flag(I2C_SR1_TXE) != 0) {
        return -1;
    }
    I2C1_DR = reg;
    return 0;
}

/* ---- Public API --------------------------------------------------------- */

int i2c_probe(uint8_t addr7)
{
    int found;

    if (ensure_bus_free() != 0) {
        return fail();
    }
    I2C1_CR1 |= I2C_CR1_START;
    if (wait_flag(I2C_SR1_SB) != 0) {
        return fail();
    }
    I2C1_DR = (uint8_t)(addr7 << 1);
    found = (wait_flag(I2C_SR1_ADDR) == 0);
    if (found) {
        clear_addr();
    }
    I2C1_CR1 |= I2C_CR1_STOP;
    I2C1_SR1 = 0U;                                  /* clear AF from a NACK */
    if (wait_stop_sent() != 0) {
        return fail();
    }
    return found ? 0 : -1;
}

int i2c_write(uint8_t addr7, uint8_t reg, const uint8_t *data, uint32_t len)
{
    if (send_register(addr7, reg) != 0) {
        return fail();
    }
    while (len-- != 0U) {
        if (wait_flag(I2C_SR1_TXE) != 0) {
            return fail();
        }
        I2C1_DR = *data++;
    }
    if (wait_flag(I2C_SR1_BTF) != 0) {
        return fail();
    }
    I2C1_CR1 |= I2C_CR1_STOP;
    return (wait_stop_sent() == 0) ? 0 : fail();
}

int i2c_read(uint8_t addr7, uint8_t reg, uint8_t *buf, uint32_t len)
{
    uint32_t pm;

    if (len == 0U) {
        return 0;
    }
    if (send_register(addr7, reg) != 0) {
        return fail();
    }
    if (wait_flag(I2C_SR1_BTF) != 0) {
        return fail();
    }

    /* ACK/POS must be prepared before the address phase of the read */
    if (len == 1U) {
        I2C1_CR1 &= ~I2C_CR1_ACK;
    } else if (len == 2U) {
        I2C1_CR1 |= I2C_CR1_ACK | I2C_CR1_POS;
    } else {
        I2C1_CR1 |= I2C_CR1_ACK;
    }

    /* Repeated START + address(read) */
    if (send_address((uint8_t)((addr7 << 1) | 1U)) != 0) {
        return fail();
    }

    if (len == 1U) {
        pm = irq_save();
        clear_addr();
        I2C1_CR1 |= I2C_CR1_STOP;
        irq_restore(pm);
        if (wait_flag(I2C_SR1_RXNE) != 0) {
            return fail();
        }
        buf[0] = (uint8_t)I2C1_DR;
    } else if (len == 2U) {
        pm = irq_save();
        clear_addr();
        I2C1_CR1 &= ~I2C_CR1_ACK;
        irq_restore(pm);
        if (wait_flag(I2C_SR1_BTF) != 0) {
            return fail();
        }
        pm = irq_save();
        I2C1_CR1 |= I2C_CR1_STOP;
        buf[0] = (uint8_t)I2C1_DR;
        irq_restore(pm);
        buf[1] = (uint8_t)I2C1_DR;
    } else {
        clear_addr();
        while (len > 3U) {
            if (wait_flag(I2C_SR1_RXNE) != 0) {
                return fail();
            }
            *buf++ = (uint8_t)I2C1_DR;
            len--;
        }
        /* 3 bytes left: wait until byte N-2 is in DR and N-1 in the shifter */
        if (wait_flag(I2C_SR1_BTF) != 0) {
            return fail();
        }
        I2C1_CR1 &= ~I2C_CR1_ACK;                   /* NACK the last byte */
        pm = irq_save();
        *buf++ = (uint8_t)I2C1_DR;                  /* N-2 */
        I2C1_CR1 |= I2C_CR1_STOP;
        *buf++ = (uint8_t)I2C1_DR;                  /* N-1 */
        irq_restore(pm);
        if (wait_flag(I2C_SR1_RXNE) != 0) {
            return fail();
        }
        *buf = (uint8_t)I2C1_DR;                    /* N   */
    }

    I2C1_CR1 &= ~I2C_CR1_POS;
    return (wait_stop_sent() == 0) ? 0 : fail();
}
