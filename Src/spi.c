/**
 * @file  spi.c
 * @brief Polling SPI1 master driver (RM0008 section 25).
 *
 * Transmit-only streaming keeps the CPU and the shift register busy at the
 * same time: spi1_send() only waits for TXE (transmit buffer empty), so the
 * caller can prepare the next frame while the current one is shifted out.
 * The received bytes are ignored in that mode; spi1_transfer() discards any
 * stale data first, then does one full-duplex byte.
 */
#include "spi.h"
#include "board.h"
#include "stm32f103_regs.h"

/* A 16-bit frame takes 1.3 us at 12 MHz (64 CPU cycles). This bound is far
 * above that and only stops the program from hanging on a broken peripheral. */
#define SPI_TIMEOUT_LOOPS   2000U

static uint32_t s_errors;

static int wait_flag(uint32_t mask, uint32_t want)
{
    for (uint32_t n = 0; n < SPI_TIMEOUT_LOOPS; n++) {
        if ((SPI1_SR & mask) == want) {
            return 0;
        }
    }
    s_errors++;
    return -1;
}

void spi1_init(void)
{
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_SPI1EN;
    RCC_APB2RSTR |= RCC_APB2RSTR_SPI1RST;
    RCC_APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;

    /* SCK and MOSI are driven by the SPI peripheral (alternate function
     * push-pull); MISO is a plain input (RM0008 "GPIO configurations for
     * device peripherals", SPI table). The pull-down on MISO makes a missing
     * SDO wire read as 0 instead of noise. */
    gpio_config(GPIOA_BASE, LCD_SCK_PIN,  GPIO_MODE_AF_PP_50MHZ);
    gpio_config(GPIOA_BASE, LCD_MOSI_PIN, GPIO_MODE_AF_PP_50MHZ);
    GPIO_BRR(GPIOA_BASE) = 1UL << LCD_MISO_PIN;          /* ODR = 0 -> pull-down */
    gpio_config(GPIOA_BASE, LCD_MISO_PIN, GPIO_MODE_IN_PULL);

    /* Master, mode 0, 48 MHz / 4 = 12 MHz (the ST7789V accepts much more,
     * but 12 MHz stays clean on breadboard jumper wires). SSM + SSI: the NSS
     * input is ignored and held high internally, otherwise the SPI would drop
     * out of master mode (MODF) because PA4 is used as an ordinary GPIO CS. */
    SPI1_CR1 = SPI_CR1_MSTR | SPI_CR1_BR_DIV4 | SPI_CR1_SSM | SPI_CR1_SSI;
    SPI1_CR2 = 0U;
    SPI1_CR1 |= SPI_CR1_SPE;
}

int spi1_flush(void)
{
    /* RM0008 "Disabling the SPI": wait for TXE = 1, then BSY = 0, before
     * touching CS or disabling the SPI; otherwise the last frame is cut short. */
    if (wait_flag(SPI_SR_TXE, SPI_SR_TXE) != 0) {
        return -1;
    }
    return wait_flag(SPI_SR_BSY, 0U);
}

int spi1_frame16(int on)
{
    uint32_t want = on ? SPI_CR1_DFF : 0U;

    if ((SPI1_CR1 & SPI_CR1_DFF) == want) {
        return 0;
    }
    /* DFF may only change while the SPI is disabled (RM0008, SPI_CR1). */
    int err = spi1_flush();
    SPI1_CR1 &= ~SPI_CR1_SPE;
    SPI1_CR1 = (SPI1_CR1 & ~SPI_CR1_DFF) | want;
    SPI1_CR1 |= SPI_CR1_SPE;
    return err;
}

int spi1_send(uint16_t frame)
{
    if (wait_flag(SPI_SR_TXE, SPI_SR_TXE) != 0) {
        return -1;
    }
    SPI1_DR = frame;
    return 0;
}

int spi1_transfer(uint8_t tx, uint8_t *rx)
{
    if (spi1_flush() != 0) {
        return -1;
    }
    /* Reading DR then SR clears RXNE and the overrun flag left behind by
     * transmit-only streaming (RM0008 "Error flags", overrun). */
    (void)SPI1_DR;
    (void)SPI1_SR;

    SPI1_DR = tx;
    if (wait_flag(SPI_SR_RXNE, SPI_SR_RXNE) != 0) {
        return -1;
    }
    *rx = (uint8_t)SPI1_DR;
    return 0;
}

uint32_t spi1_error_count(void)
{
    return s_errors;
}
