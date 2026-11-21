/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : MPU6050 data acquisition on an STM32F103C8T6 (Blue Pill)
 ******************************************************************************
 * Wiring (HARDWARE.md):
 *   PB6  SCL  / PB7  SDA  -> I2C1, 100 kHz
 *   PB12 INT  <- data-ready / motion interrupt (active high, latched)
 *   PB13 AD0  -> I2C address select, driven low (0x68) by default
 *   PB10 XCL  / PB11 XDA  -> MPU6050 auxiliary bus, left high-impedance
 *   PC13 LED  -> heartbeat: slow blink = sensor OK, fast blink = sensor missing
 *
 * Every new sample is published in g_telemetry (see telemetry.h), which the
 * PC reads through the ST-Link with tools/mpu6050_monitor.py.
 ******************************************************************************
 */

#include <stdint.h>
#include "board.h"
#include "i2c.h"
#include "mpu6050.h"
#include "stm32f103_regs.h"
#include "telemetry.h"
#include "timebase.h"

#define BLINK_PERIOD_MS       1500U   /* heartbeat while the MPU6050 works   */
#define BLINK_ERROR_MS        100U    /* fast blink: MPU6050 not responding  */
#define INT_FALLBACK_POLL_MS  5U      /* poll INT_STATUS if PB12 stays low   */
#define REG_DUMP_PERIOD_MS    1000U
#define RETRY_PERIOD_MS       1000U
#define MAX_READ_FAILURES     10U

#define COMPILER_BARRIER()    __asm volatile ("" ::: "memory")

__attribute__((used)) volatile telemetry_t g_telemetry;

static mpu6050_t s_mpu = {
    .addr       = MPU6050_ADDR_AD0_LOW,
    .accel_fs   = 0U,     /* +-2 g       */
    .gyro_fs    = 0U,     /* +-250 deg/s */
    .dlpf       = 3U,     /* 44 Hz accel / 42 Hz gyro bandwidth */
    .smplrt_div = 9U,     /* 1 kHz / (1 + 9) = 100 Hz */
};

static uint8_t  s_mpu_ok;
static uint8_t  s_ad0_level;
static uint32_t s_read_failures;
static uint32_t s_last_sample_ms;
static uint32_t s_last_poll_ms;
static uint32_t s_last_dump_ms;
static uint32_t s_last_retry_ms;
static uint32_t s_last_cmd_seq;
static uint8_t  s_regs[128];

/* ---- Telemetry helpers -------------------------------------------------- */

static void tm_begin(void)
{
    g_telemetry.seq_tail = g_telemetry.seq_tail + 1U;
    COMPILER_BARRIER();
}

static void tm_end(void)
{
    g_telemetry.i2c_errors = i2c_error_count();
    COMPILER_BARRIER();
    g_telemetry.seq_head = g_telemetry.seq_tail;
}

static void tm_publish_config(void)
{
    g_telemetry.i2c_addr   = s_mpu.addr;
    g_telemetry.who_am_i   = s_mpu.who_am_i;
    g_telemetry.accel_fs   = s_mpu.accel_fs;
    g_telemetry.gyro_fs    = s_mpu.gyro_fs;
    g_telemetry.dlpf       = s_mpu.dlpf;
    g_telemetry.smplrt_div = s_mpu.smplrt_div;
    g_telemetry.ad0_level  = s_ad0_level;
    if (s_mpu_ok) {
        g_telemetry.status |= TM_STATUS_MPU_OK;
    } else {
        g_telemetry.status &= (uint8_t)~TM_STATUS_MPU_OK;
    }
}

static void telemetry_init(void)
{
    g_telemetry.version = TELEMETRY_VERSION;
    g_telemetry.size    = (uint16_t)sizeof(telemetry_t);
    COMPILER_BARRIER();
    g_telemetry.magic   = TELEMETRY_MAGIC;
}

/* ---- Board -------------------------------------------------------------- */

static void board_init(void)
{
    RCC_APB2ENR |= RCC_APB2ENR_IOPBEN | RCC_APB2ENR_IOPCEN | RCC_APB2ENR_AFIOEN;

    gpio_config(GPIOC_BASE, LED_PIN, GPIO_MODE_OUT_PP_2MHZ);

    /* AD0 low -> I2C address 0x68 */
    GPIO_BRR(GPIOB_BASE) = 1UL << MPU_AD0_PIN;
    gpio_config(GPIOB_BASE, MPU_AD0_PIN, GPIO_MODE_OUT_PP_2MHZ);

    /* INT: input with pull-down (ODR bit = 0 selects pull-down) */
    GPIO_BRR(GPIOB_BASE) = 1UL << MPU_INT_PIN;
    gpio_config(GPIOB_BASE, MPU_INT_PIN, GPIO_MODE_IN_PULL);

    /* XCL/XDA belong to the MPU6050's own auxiliary I2C master; the STM32
     * must not drive them, so keep them as high-impedance inputs. */
    gpio_config(GPIOB_BASE, MPU_XCL_PIN, GPIO_MODE_IN_FLOATING);
    gpio_config(GPIOB_BASE, MPU_XDA_PIN, GPIO_MODE_IN_FLOATING);
}

static void set_ad0(uint8_t level)
{
    s_ad0_level = level ? 1U : 0U;
    if (s_ad0_level) {
        GPIO_BSRR(GPIOB_BASE) = 1UL << MPU_AD0_PIN;
    } else {
        GPIO_BRR(GPIOB_BASE) = 1UL << MPU_AD0_PIN;
    }
}

/* ---- MPU6050 handling --------------------------------------------------- */

static void run_self_test(void)
{
    mpu6050_selftest_t st;

    if (mpu6050_self_test(&s_mpu, &st) != 0) {
        return;
    }
    tm_begin();
    for (int i = 0; i < 3; i++) {
        g_telemetry.st_accel[i] = st.accel_response[i];
        g_telemetry.st_gyro[i]  = st.gyro_response[i];
    }
    for (int i = 0; i < 4; i++) {
        g_telemetry.st_trim[i] = st.factory_trim[i];
    }
    g_telemetry.selftest_count = g_telemetry.selftest_count + 1U;
    g_telemetry.status |= TM_STATUS_SELFTEST_OK;
    tm_end();
}

/* Find the sensor, initialise it and run the self-test. */
static void mpu_start(void)
{
    uint8_t wanted = (uint8_t)(MPU6050_ADDR_AD0_LOW | s_ad0_level);

    delay_ms(5);
    s_mpu_ok = 0U;
    if (i2c_probe(wanted) == 0) {
        s_mpu.addr = wanted;
    } else if (i2c_probe((uint8_t)(wanted ^ 1U)) == 0) {
        s_mpu.addr = (uint8_t)(wanted ^ 1U);    /* AD0 wire has no effect */
    } else {
        tm_begin();
        tm_publish_config();
        tm_end();
        return;
    }

    s_mpu_ok = (mpu6050_init(&s_mpu) == 0) ? 1U : 0U;
    s_read_failures = 0U;

    tm_begin();
    tm_publish_config();
    tm_end();

    if (s_mpu_ok) {
        run_self_test();
    }
}

static void read_failed(void)
{
    if (++s_read_failures >= MAX_READ_FAILURES) {
        s_mpu_ok = 0U;
        tm_begin();
        tm_publish_config();
        tm_end();
    }
}

static void poll_sensor(uint32_t now)
{
    mpu6050_sample_t s;
    uint8_t int_status;
    uint32_t pin = (GPIO_IDR(GPIOB_BASE) >> MPU_INT_PIN) & 1UL;

    /* Read when PB12 signals new data; poll slowly as a fallback so the
     * system also works if the INT wire is missing. */
    if (!pin && (now - s_last_poll_ms) < INT_FALLBACK_POLL_MS) {
        return;
    }
    s_last_poll_ms = now;

    /* Reading INT_STATUS clears the latched interrupt (PB12 goes low) */
    if (mpu6050_read_regs(&s_mpu, MPU_REG_INT_STATUS, &int_status, 1U) != 0) {
        read_failed();
        return;
    }
    if ((int_status & (MPU_INT_DATA_RDY | MPU_INT_MOT)) == 0U) {
        return;
    }

    int have_sample = 0;
    if (int_status & MPU_INT_DATA_RDY) {
        if (mpu6050_read_sample(&s_mpu, &s) == 0) {
            have_sample = 1;
            s_read_failures = 0U;
        } else {
            read_failed();
        }
    }

    tm_begin();
    g_telemetry.int_status    = int_status;
    g_telemetry.int_pin_level = (uint8_t)pin;
    if (int_status & MPU_INT_MOT) {
        g_telemetry.motion_count = g_telemetry.motion_count + 1U;
    }
    if (have_sample) {
        int32_t dt = (int32_t)(now - s_last_sample_ms);
        s_last_sample_ms = now;

        for (int i = 0; i < 3; i++) {
            g_telemetry.accel[i] = s.accel[i];
            g_telemetry.gyro[i]  = s.gyro[i];
            /* Gyro integral (raw * ms): lets the PC integrate the angle
             * exactly even if it reads slower than the sample rate. */
            g_telemetry.gyro_int[i] = g_telemetry.gyro_int[i] + (uint32_t)((int32_t)s.gyro[i] * dt);
        }
        g_telemetry.temp           = s.temp;
        g_telemetry.sample_time_ms = now;
        g_telemetry.sample_count   = g_telemetry.sample_count + 1U;
        if (pin) {
            g_telemetry.int_pin_count = g_telemetry.int_pin_count + 1U;
            g_telemetry.status |= TM_STATUS_VIA_INT_PIN;
        } else {
            g_telemetry.status &= (uint8_t)~TM_STATUS_VIA_INT_PIN;
        }
    }
    tm_end();
}

/* Snapshot of every documented register. INT_STATUS (0x3A, clears on read),
 * the DMP memory window (0x6D-0x71) and FIFO_R_W (0x74, pops the FIFO) are
 * skipped on purpose. */
static void dump_registers(void)
{
    static const struct { uint8_t start; uint8_t len; } ranges[] = {
        { 0x00U, 3U },                       /* MPU6500 gyro self-test codes */
        { 0x0DU, 0x39U - 0x0DU + 1U },
        { 0x3BU, 0x6CU - 0x3BU + 1U },
        { 0x72U, 2U },
        { 0x75U, 1U },
    };

    for (uint32_t r = 0; r < sizeof ranges / sizeof ranges[0]; r++) {
        if (mpu6050_read_regs(&s_mpu, ranges[r].start, &s_regs[ranges[r].start], ranges[r].len) != 0) {
            read_failed();
            return;
        }
    }

    tm_begin();
    s_regs[MPU_REG_INT_STATUS] = g_telemetry.int_status;
    for (uint32_t i = 0; i < sizeof s_regs; i++) {
        g_telemetry.regs[i] = s_regs[i];
    }
    tm_end();
}

static void handle_command(void)
{
    uint32_t seq = g_telemetry.cmd_seq;
    if (seq == s_last_cmd_seq) {
        return;
    }
    s_last_cmd_seq = seq;

    uint8_t id  = g_telemetry.cmd_id;
    uint8_t arg = g_telemetry.cmd_arg;

    switch (id) {
    case TM_CMD_SET_ACCEL_FS:
        s_mpu.accel_fs = arg & 0x03U;
        (void)mpu6050_apply_config(&s_mpu);
        break;
    case TM_CMD_SET_GYRO_FS:
        s_mpu.gyro_fs = arg & 0x03U;
        (void)mpu6050_apply_config(&s_mpu);
        break;
    case TM_CMD_SET_DLPF:
        if (arg <= 6U) {
            s_mpu.dlpf = arg;
            (void)mpu6050_apply_config(&s_mpu);
        }
        break;
    case TM_CMD_SET_SMPLRT_DIV:
        s_mpu.smplrt_div = arg;
        (void)mpu6050_apply_config(&s_mpu);
        break;
    case TM_CMD_SELF_TEST:
        if (s_mpu_ok) {
            run_self_test();
        }
        break;
    case TM_CMD_REINIT:
        mpu_start();
        break;
    case TM_CMD_SET_AD0:
        set_ad0(arg);
        mpu_start();
        break;
    default:
        break;
    }

    tm_begin();
    tm_publish_config();
    g_telemetry.cmd_ack = seq;
    tm_end();
}

static void heartbeat(uint32_t now)
{
    static uint32_t last_toggle;
    uint32_t period = s_mpu_ok ? BLINK_PERIOD_MS : BLINK_ERROR_MS;

    if ((now - last_toggle) >= period) {
        last_toggle = now;
        GPIO_ODR(GPIOC_BASE) ^= 1UL << LED_PIN;
    }
}

int main(void)
{
    DBGMCU_CR |= DBGMCU_CR_DBG_SLEEP;   /* ST-Link can read RAM during WFI */

    timebase_init();
    board_init();
    i2c_init();
    telemetry_init();
    mpu_start();

    for (;;) {
        uint32_t now = millis();

        g_telemetry.uptime_ms = now;
        handle_command();

        if (s_mpu_ok) {
            poll_sensor(now);
            if ((now - s_last_dump_ms) >= REG_DUMP_PERIOD_MS) {
                s_last_dump_ms = now;
                dump_registers();
            }
        } else if ((now - s_last_retry_ms) >= RETRY_PERIOD_MS) {
            s_last_retry_ms = now;
            mpu_start();
        }

        heartbeat(now);
        /* No WFI here: the ST-Link must be able to read RAM at any time,
         * and reads during sleep returned zeros on this board. */
    }
}
