/**
 * @file  dfu_bootloader.c
 * @brief USB DFU 1.1 bootloader (8 KB at 0x08000000) for the Blue Pill.
 *
 * After every reset this runs first and decides:
 *
 *   BKP_DR1 == BOOT_REQUEST_MAGIC  (the application asked, e.g. 1200 baud)  -> DFU mode
 *   BOOT1 jumper = 1 (PB2 high)    (manual recovery)                         -> DFU mode
 *   no valid application at 0x08002000                                       -> DFU mode
 *   otherwise                                                                -> jump to the app
 *
 * In DFU mode the board enumerates as 1209:0002 and `dfu-util` can write the
 * application (DFU_DNLOAD) or read it back (DFU_UPLOAD), 1 KB (one flash
 * page) per block. Block n goes to APP_BASE + n * 1024.
 *
 * Safe against interrupted updates: block 0 holds the application's vector
 * table. Its page is erased first (the app becomes "invalid") and written
 * last, during manifestation. A half-written image therefore never looks
 * valid and the bootloader stays in DFU mode instead of jumping into it.
 *
 * The LED on PC13 blinks quickly while in DFU mode.
 *
 * References: USB Device Class Specification for Device Firmware Upgrade 1.1,
 * RM0008 "Embedded Flash memory" (FPEC programming), PM0056 (VTOR, MSP).
 */
#include <stdint.h>

#include "board.h"
#include "stm32f103_regs.h"
#include "timebase.h"
#include "usb.h"
#include "usb_ids.h"

#define TRANSFER_SIZE     1024U          /* = flash page size on the F103x8/xB */
#define FLASH_TIMEOUT     2000000UL      /* loops; a page erase takes 20-40 ms */
#define BOOT1_PIN         2U             /* PB2, the Blue Pill's BOOT1 jumper   */
#define RAM_START         0x20000000UL
#define RAM_END           (RAM_START + 20UL * 1024UL)

/* Flash is read and programmed through plain pointers. These macros only
 * exist so tools/usb_sim can run this file on a PC against simulated flash. */
#ifndef FLASH_MEM8
#define FLASH_MEM8(a)     (*(volatile const uint8_t *)(a))
#define FLASH_MEM16(a)    (*(volatile uint16_t *)(a))
#define FLASH_MEM32(a)    (*(volatile const uint32_t *)(a))
#endif

/* DFU class requests (DFU 1.1, table 3.2) */
enum { DFU_DETACH, DFU_DNLOAD, DFU_UPLOAD, DFU_GETSTATUS, DFU_CLRSTATUS, DFU_GETSTATE, DFU_ABORT };

/* DFU states and status codes (DFU 1.1, section 6.1.2) */
enum {
    appIDLE, appDETACH, dfuIDLE, dfuDNLOAD_SYNC, dfuDNBUSY, dfuDNLOAD_IDLE,
    dfuMANIFEST_SYNC, dfuMANIFEST, dfuMANIFEST_WAIT_RESET, dfuUPLOAD_IDLE, dfuERROR
};
enum { OK = 0x00, errWRITE = 0x03, errERASE = 0x04, errVERIFY = 0x07, errADDRESS = 0x08, errUNKNOWN = 0x0E };

#define LO(x)  ((uint8_t)((x) & 0xFFU))
#define HI(x)  ((uint8_t)((x) >> 8))

static const uint8_t s_device_desc[18] = {
    18, 0x01, 0x00, 0x02,          /* USB 2.00                                    */
    0x00, 0x00, 0x00, 64,          /* class per interface, EP0 = 64 bytes         */
    LO(USB_VID), HI(USB_VID), LO(USB_PID_DFU), HI(USB_PID_DFU),
    0x00, 0x01, 1, 2, 3, 1
};

#define CONFIG_LEN  27U
static const uint8_t s_config_desc[CONFIG_LEN] = {
    9, 0x02, LO(CONFIG_LEN), HI(CONFIG_LEN), 1, 1, 0, 0x80, 50,      /* bus powered, 100 mA */
    9, 0x04, 0, 0, 0, 0xFE, 0x01, 0x02, 4,   /* application specific, DFU, DFU mode, string 4 */
    /* DFU functional descriptor: can download + upload, manifestation
     * tolerant (stays answering after the image is written), detach
     * timeout 255 ms, 1024-byte blocks, DFU 1.1 */
    9, 0x21, 0x07, 255, 0, LO(TRANSFER_SIZE), HI(TRANSFER_SIZE), 0x10, 0x01,
};

static const char *const s_strings[] = {
    0, "MPU6050-and-STM32", "MPU6050 DFU bootloader", 0, "Application @0x08002000",
};

static volatile uint8_t s_state = dfuIDLE;
static volatile uint8_t s_status = OK;
static uint8_t  s_block[TRANSFER_SIZE];          /* block being written         */
static uint16_t s_block_len, s_block_num;
static uint8_t  s_first[TRANSFER_SIZE];          /* block 0, written last       */
static uint16_t s_first_len;
static volatile uint8_t s_work;                  /* job for the main loop       */
static volatile uint8_t s_manifested;
static volatile uint8_t s_reboot;
enum { WORK_NONE, WORK_PROGRAM, WORK_MANIFEST };

static uint32_t flash_end(void)
{
    return 0x08000000UL + (uint32_t)FLASH_SIZE_KB * 1024UL;
}

/* ---- Flash programming (FPEC) ---------------------------------------------- */

static int flash_wait(void)
{
    for (uint32_t n = 0; n < FLASH_TIMEOUT; n++) {
        if ((FLASH_SR & FLASH_SR_BSY) == 0U) {
            uint32_t sr = FLASH_SR;
            FLASH_SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;   /* write 1 to clear */
            return (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) ? -1 : 0;
        }
    }
    return -1;
}

static void flash_unlock(void)
{
    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
}

static int flash_erase_page(uint32_t addr)
{
    flash_unlock();
    FLASH_CR |= FLASH_CR_PER;
    FLASH_AR = addr;
    FLASH_CR |= FLASH_CR_STRT;            /* the CPU stalls while it erases */
    int err = flash_wait();
    FLASH_CR &= ~FLASH_CR_PER;
    return err;
}

/* The F103 programs 16 bits at a time; an odd last byte is padded with 0xFF. */
static int flash_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    int err = 0;
    flash_unlock();
    FLASH_CR |= FLASH_CR_PG;
    for (uint32_t i = 0; (i < len) && (err == 0); i += 2U) {
        uint16_t v = (uint16_t)(data[i] | (((i + 1U) < len ? data[i + 1U] : 0xFFU) << 8));
        FLASH_MEM16(addr + i) = v;
        err = flash_wait();
    }
    FLASH_CR &= ~FLASH_CR_PG;
    for (uint32_t i = 0; (i < len) && (err == 0); i++) {        /* verify */
        if (FLASH_MEM8(addr + i) != data[i]) {
            err = -2;
        }
    }
    return err;
}

/* ---- DFU work done in the main loop (flash operations stall the CPU, so they
 *      run after the GETSTATUS reply has been sent) ------------------------ */

static void do_program(void)
{
    uint32_t addr = APP_BASE + (uint32_t)s_block_num * TRANSFER_SIZE;

    if ((addr < APP_BASE) || (addr + s_block_len > flash_end())) {
        s_status = errADDRESS;
    } else if (s_block_num == 0U) {
        /* Keep the vector table for the end; erasing its page now makes the
         * old application invalid until the new image is complete. */
        for (uint32_t i = 0; i < s_block_len; i++) {
            s_first[i] = s_block[i];
        }
        s_first_len = s_block_len;
        s_status = (flash_erase_page(addr) == 0) ? OK : errERASE;
    } else if (flash_erase_page(addr) != 0) {
        s_status = errERASE;
    } else {
        int r = flash_program(addr, s_block, s_block_len);
        s_status = (r == 0) ? OK : (r == -2 ? errVERIFY : errWRITE);
    }
    FLASH_CR |= FLASH_CR_LOCK;
    s_state = (s_status == OK) ? dfuDNLOAD_IDLE : dfuERROR;
}

static void do_manifest(void)
{
    s_status = OK;
    if (s_first_len != 0U) {
        int r = flash_program(APP_BASE, s_first, s_first_len);
        s_status = (r == 0) ? OK : (r == -2 ? errVERIFY : errWRITE);
        s_first_len = 0U;
    }
    FLASH_CR |= FLASH_CR_LOCK;
    s_manifested = (s_status == OK) ? 1U : 0U;
    s_state = (s_status == OK) ? dfuMANIFEST_SYNC : dfuERROR;
}

/* ---- DFU class requests (run from usb_poll) ------------------------------- */

static int dfu_setup(const usb_setup_t *req, uint8_t *buf, uint16_t *len)
{
    if ((req->bmRequestType & USB_REQ_TYPE_MASK) != USB_REQ_TYPE_CLASS) {
        return USB_STALL;
    }
    switch (req->bRequest) {
    case DFU_DNLOAD:
        if ((s_state != dfuIDLE) && (s_state != dfuDNLOAD_IDLE)) {
            s_state = dfuERROR;
            s_status = errUNKNOWN;
            return USB_STALL;
        }
        if (req->wLength == 0U) {                 /* "end of image" */
            if (s_state != dfuDNLOAD_IDLE) {
                return USB_STALL;
            }
            s_state = dfuMANIFEST_SYNC;
            s_manifested = 0U;
            return USB_OK;
        }
        return (req->wLength <= TRANSFER_SIZE) ? USB_OK : USB_STALL;   /* data follows */

    case DFU_UPLOAD: {
        if ((s_state != dfuIDLE) && (s_state != dfuUPLOAD_IDLE)) {
            return USB_STALL;
        }
        uint32_t addr = APP_BASE + (uint32_t)req->wValue * TRANSFER_SIZE;
        uint32_t n = (addr < flash_end()) ? (flash_end() - addr) : 0U;
        if (n > req->wLength) {
            n = req->wLength;
        }
        for (uint32_t i = 0; i < n; i++) {
            buf[i] = FLASH_MEM8(addr + i);
        }
        *len = (uint16_t)n;
        s_state = (n < req->wLength) ? dfuIDLE : dfuUPLOAD_IDLE;   /* short block = end */
        return USB_OK;
    }

    case DFU_GETSTATUS: {
        uint32_t poll_ms = 0U;
        if (s_state == dfuDNLOAD_SYNC) {
            s_state = dfuDNBUSY;
            s_work = WORK_PROGRAM;
            poll_ms = 100U;                           /* erase + program 1 KB < 100 ms */
        } else if ((s_state == dfuMANIFEST_SYNC) && !s_manifested) {
            s_state = dfuMANIFEST;
            s_work = WORK_MANIFEST;
            poll_ms = 100U;
        } else if (s_state == dfuMANIFEST_SYNC) {
            s_state = dfuIDLE;                        /* manifestation tolerant */
        }
        buf[0] = s_status;
        buf[1] = (uint8_t)poll_ms;
        buf[2] = (uint8_t)(poll_ms >> 8);
        buf[3] = (uint8_t)(poll_ms >> 16);
        buf[4] = s_state;
        buf[5] = 0U;
        *len = 6U;
        return USB_OK;
    }

    case DFU_CLRSTATUS:
        if (s_state == dfuERROR) {
            s_state = dfuIDLE;
            s_status = OK;
        }
        return USB_OK;

    case DFU_GETSTATE:
        buf[0] = s_state;
        *len = 1U;
        return USB_OK;

    case DFU_ABORT:
        s_state = dfuIDLE;
        s_first_len = 0U;
        return USB_OK;

    case DFU_DETACH:                                  /* dfu-util -R sends this */
        if (s_manifested) {
            s_reboot = 1U;
        }
        return USB_OK;

    default:
        return USB_STALL;
    }
}

static int dfu_data_out(const usb_setup_t *req, const uint8_t *buf, uint16_t len)
{
    if (req->bRequest != DFU_DNLOAD) {
        return USB_STALL;
    }
    for (uint32_t i = 0; i < len; i++) {
        s_block[i] = buf[i];
    }
    s_block_len = len;
    s_block_num = req->wValue;
    s_state = dfuDNLOAD_SYNC;
    return USB_OK;
}

static void dfu_bus_reset(void)
{
    if (s_manifested) {                  /* new image written, host reset us: start it */
        s_reboot = 1U;
    }
}

static const usb_class_t s_dfu = {
    .device_desc = s_device_desc,
    .config_desc = s_config_desc,
    .strings     = s_strings,
    .num_strings = (uint8_t)(sizeof s_strings / sizeof s_strings[0]),
    .setup       = dfu_setup,
    .data_out    = dfu_data_out,
    .status_done = 0,
    .configured  = 0,
    .ep_event    = 0,
    .bus_reset   = dfu_bus_reset,
};

/* ---- Start-up decision ------------------------------------------------------- */

static int app_is_valid(void)
{
    uint32_t sp = FLASH_MEM32(APP_BASE);
    uint32_t pc = FLASH_MEM32(APP_BASE + 4U);
    return (sp > RAM_START) && (sp <= RAM_END) &&
           (pc >= APP_BASE) && (pc < flash_end()) && ((pc & 1U) != 0U);    /* Thumb */
}

static int dfu_requested(void)
{
    RCC_APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    if ((BKP_DR1 & 0xFFFFU) == BOOT_REQUEST_MAGIC) {
        PWR_CR |= PWR_CR_DBP;
        BKP_DR1 = 0U;                    /* one-shot: the next reset starts the app */
        PWR_CR &= ~PWR_CR_DBP;
        return 1;
    }
    RCC_APB2ENR |= RCC_APB2ENR_IOPBEN;   /* PB2 is a floating input after reset */
    for (volatile uint32_t n = 0; n < 1000U; n++) {
    }
    return (int)((GPIO_IDR(GPIOB_BASE) >> BOOT1_PIN) & 1UL);
}

/* Jump with the reset state of the core: the application's own SystemInit()
 * re-does the clocks and points VTOR at its vector table. */
/* One pass of the DFU main loop: flash work, delayed reboot, LED. */
static void dfu_task(void)
{
    static uint32_t reboot_at;

    if (s_work == WORK_PROGRAM) {
        s_work = WORK_NONE;
        do_program();
    } else if (s_work == WORK_MANIFEST) {
        s_work = WORK_NONE;
        do_manifest();
    }

    uint32_t now = millis();
    if (s_reboot && (reboot_at == 0U)) {
        reboot_at = now + 50U;           /* finish the current USB transfer first */
    }
    if ((reboot_at != 0U) && ((int32_t)(now - reboot_at) >= 0)) {
        usb_stop();
        system_reset();                  /* the bootloader runs again and starts the app */
    }

    /* Quick double blink: "I am the bootloader" */
    uint32_t phase = now % 1000U;
    if ((phase < 80U) || ((phase >= 200U) && (phase < 280U))) {
        GPIO_BRR(GPIOC_BASE) = 1UL << LED_PIN;    /* LED is active low */
    } else {
        GPIO_BSRR(GPIOC_BASE) = 1UL << LED_PIN;
    }
}

int main(void)
{
    if (!dfu_requested() && app_is_valid()) {
        /* Jump with the reset state of the core: the application's own
         * SystemInit() re-does the clocks and points VTOR at its vectors. */
        start_image(FLASH_MEM32(APP_BASE), FLASH_MEM32(APP_BASE + 4U));
    }

    timebase_init();
    RCC_APB2ENR |= RCC_APB2ENR_IOPCEN;
    gpio_config(GPIOC_BASE, LED_PIN, GPIO_MODE_OUT_PP_2MHZ);
    usb_init(&s_dfu);

    for (;;) {
        usb_poll();
        dfu_task();
    }
}
