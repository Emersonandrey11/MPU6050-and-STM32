/**
 * @file  mpu6050.c
 * @brief MPU6050 6-axis IMU driver (register map rev 4.2).
 */
#include "mpu6050.h"
#include "i2c.h"
#include "timebase.h"

#define ACCEL_HPF_5HZ       0x01U   /* high-pass for motion detection only  */
#define SELF_TEST_SAMPLES   32

static int16_t be16(const uint8_t *p)
{
    return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int mpu6050_write_reg(const mpu6050_t *dev, uint8_t reg, uint8_t value)
{
    return i2c_write(dev->addr, reg, &value, 1U);
}

int mpu6050_read_regs(const mpu6050_t *dev, uint8_t reg, uint8_t *buf, uint32_t len)
{
    return i2c_read(dev->addr, reg, buf, len);
}

int mpu6050_read_sample(const mpu6050_t *dev, mpu6050_sample_t *s)
{
    uint8_t b[14];

    if (i2c_read(dev->addr, MPU_REG_ACCEL_XOUT_H, b, sizeof b) != 0) {
        return -1;
    }
    for (int i = 0; i < 3; i++) {
        s->accel[i] = be16(&b[2 * i]);
        s->gyro[i]  = be16(&b[8 + 2 * i]);
    }
    s->temp = be16(&b[6]);
    return 0;
}

static int is_6500_family(uint8_t who_am_i)
{
    return (who_am_i == 0x70U) || (who_am_i == 0x71U) || (who_am_i == 0x73U) || (who_am_i == 0x74U);
}

int mpu6050_apply_config(const mpu6050_t *dev)
{
    int err = 0;

    err |= mpu6050_write_reg(dev, MPU_REG_SMPLRT_DIV, dev->smplrt_div);
    err |= mpu6050_write_reg(dev, MPU_REG_CONFIG, (uint8_t)(dev->dlpf & 0x07U));
    err |= mpu6050_write_reg(dev, MPU_REG_GYRO_CONFIG, (uint8_t)((dev->gyro_fs & 0x03U) << 3));
    if (dev->is_6500) {
        /* The MPU6500 has a separate accelerometer filter; bits 2:0 of
         * ACCEL_CONFIG are reserved there. */
        err |= mpu6050_write_reg(dev, MPU_REG_ACCEL_CONFIG, (uint8_t)((dev->accel_fs & 0x03U) << 3));
        err |= mpu6050_write_reg(dev, MPU_REG_ACCEL_CONFIG2, (uint8_t)(dev->dlpf & 0x07U));
    } else {
        err |= mpu6050_write_reg(dev, MPU_REG_ACCEL_CONFIG,
                                 (uint8_t)(((dev->accel_fs & 0x03U) << 3) | ACCEL_HPF_5HZ));
    }
    return err ? -1 : 0;
}

int mpu6050_init(mpu6050_t *dev)
{
    int err = 0;

    err |= mpu6050_write_reg(dev, MPU_REG_PWR_MGMT_1, 0x80U);       /* DEVICE_RESET */
    delay_ms(100);
    err |= mpu6050_write_reg(dev, MPU_REG_SIGNAL_PATH_RST, 0x07U);  /* gyro/accel/temp */
    delay_ms(100);
    err |= mpu6050_write_reg(dev, MPU_REG_PWR_MGMT_1, 0x01U);       /* wake, PLL from gyro X */
    err |= mpu6050_write_reg(dev, MPU_REG_PWR_MGMT_2, 0x00U);       /* all axes on */
    err |= mpu6050_write_reg(dev, MPU_REG_USER_CTRL, 0x00U);        /* no FIFO, no aux master */
    err |= mpu6050_read_regs(dev, MPU_REG_WHO_AM_I, &dev->who_am_i, 1U);
    dev->is_6500 = (uint8_t)is_6500_family(dev->who_am_i);
    err |= mpu6050_apply_config(dev);

    if (dev->is_6500) {
        /* Wake-on-motion: 10 * 4 mg = 40 mg threshold, compare to previous sample */
        err |= mpu6050_write_reg(dev, MPU_REG_MOT_THR, 10U);
        err |= mpu6050_write_reg(dev, MPU_REG_MOT_DETECT_CTRL, 0xC0U);
    } else {
        /* Motion detection: 20 * 2 mg = 40 mg threshold, 1 ms duration */
        err |= mpu6050_write_reg(dev, MPU_REG_MOT_THR, 20U);
        err |= mpu6050_write_reg(dev, MPU_REG_MOT_DUR, 1U);
        err |= mpu6050_write_reg(dev, MPU_REG_MOT_DETECT_CTRL, 0x15U);
    }

    /* INT pin: active high, push-pull, latched until INT_STATUS is read.
     * I2C bypass stays off, so XDA/XCL are not connected to SDA/SCL. */
    err |= mpu6050_write_reg(dev, MPU_REG_INT_PIN_CFG, 0x20U);
    err |= mpu6050_write_reg(dev, MPU_REG_INT_ENABLE, MPU_INT_DATA_RDY | MPU_INT_MOT);

    return err ? -1 : 0;
}

static int average_samples(const mpu6050_t *dev, int32_t accel[3], int32_t gyro[3])
{
    mpu6050_sample_t s;

    for (int i = 0; i < 3; i++) {
        accel[i] = 0;
        gyro[i] = 0;
    }
    for (int n = 0; n < SELF_TEST_SAMPLES; n++) {
        delay_ms(2);
        if (mpu6050_read_sample(dev, &s) != 0) {
            return -1;
        }
        for (int i = 0; i < 3; i++) {
            accel[i] += s.accel[i];
            gyro[i]  += s.gyro[i];
        }
    }
    for (int i = 0; i < 3; i++) {
        accel[i] /= SELF_TEST_SAMPLES;
        gyro[i]  /= SELF_TEST_SAMPLES;
    }
    return 0;
}

/* Self-test: the response is the difference between outputs with the
 * internal test actuation on and off. The host compares it with the factory
 * values: MPU6050 registers 0x0D-0x10 at +-8 g, or MPU6500 registers
 * 0x00-0x02 / 0x0D-0x0F at +-2 g (both at +-250 dps). */
int mpu6050_self_test(const mpu6050_t *dev, mpu6050_selftest_t *out)
{
    int32_t a_off[3], g_off[3], a_on[3], g_on[3];
    const uint8_t accel_fs_bits = dev->is_6500 ? 0x00U : 0x10U;    /* +-2 g / +-8 g */
    int err = 0;

    err |= mpu6050_write_reg(dev, MPU_REG_SMPLRT_DIV, 0U);
    err |= mpu6050_write_reg(dev, MPU_REG_CONFIG, 2U);
    if (dev->is_6500) {
        err |= mpu6050_write_reg(dev, MPU_REG_ACCEL_CONFIG2, 2U);
    }
    err |= mpu6050_write_reg(dev, MPU_REG_GYRO_CONFIG, 0x00U);      /* +-250 dps    */
    err |= mpu6050_write_reg(dev, MPU_REG_ACCEL_CONFIG, accel_fs_bits);
    delay_ms(50);
    err |= average_samples(dev, a_off, g_off);

    err |= mpu6050_write_reg(dev, MPU_REG_GYRO_CONFIG, 0xE0U);      /* XG/YG/ZG_ST  */
    err |= mpu6050_write_reg(dev, MPU_REG_ACCEL_CONFIG, (uint8_t)(0xE0U | accel_fs_bits));
    delay_ms(50);
    err |= average_samples(dev, a_on, g_on);

    err |= mpu6050_read_regs(dev, MPU_REG_SELF_TEST_X, out->factory_trim, 4U);

    for (int i = 0; i < 3; i++) {
        out->accel_response[i] = a_on[i] - a_off[i];
        out->gyro_response[i]  = g_on[i] - g_off[i];
    }

    err |= mpu6050_apply_config(dev);                               /* restore */
    return err ? -1 : 0;
}
