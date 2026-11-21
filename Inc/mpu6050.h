/**
 * @file  mpu6050.h
 * @brief MPU6050 6-axis IMU driver (register map rev 4.2).
 */
#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>

/* I2C addresses (7-bit), selected by the AD0 pin */
#define MPU6050_ADDR_AD0_LOW    0x68U
#define MPU6050_ADDR_AD0_HIGH   0x69U

/* Registers */
#define MPU_REG_SELF_TEST_X     0x0DU
#define MPU_REG_SELF_TEST_Y     0x0EU
#define MPU_REG_SELF_TEST_Z     0x0FU
#define MPU_REG_SELF_TEST_A     0x10U
#define MPU_REG_SMPLRT_DIV      0x19U
#define MPU_REG_CONFIG          0x1AU
#define MPU_REG_GYRO_CONFIG     0x1BU
#define MPU_REG_ACCEL_CONFIG    0x1CU
#define MPU_REG_ACCEL_CONFIG2   0x1DU   /* MPU6500 family only */
#define MPU_REG_MOT_THR         0x1FU   /* WOM_THR on the MPU6500 */
#define MPU_REG_MOT_DUR         0x20U
#define MPU_REG_FIFO_EN         0x23U
#define MPU_REG_INT_PIN_CFG     0x37U
#define MPU_REG_INT_ENABLE      0x38U
#define MPU_REG_INT_STATUS      0x3AU
#define MPU_REG_ACCEL_XOUT_H    0x3BU
#define MPU_REG_TEMP_OUT_H      0x41U
#define MPU_REG_GYRO_XOUT_H     0x43U
#define MPU_REG_SIGNAL_PATH_RST 0x68U
#define MPU_REG_MOT_DETECT_CTRL 0x69U   /* ACCEL_INTEL_CTRL on the MPU6500 */
#define MPU_REG_USER_CTRL       0x6AU
#define MPU_REG_PWR_MGMT_1      0x6BU
#define MPU_REG_PWR_MGMT_2      0x6CU
#define MPU_REG_FIFO_COUNTH     0x72U
#define MPU_REG_FIFO_R_W        0x74U
#define MPU_REG_WHO_AM_I        0x75U

/* INT_STATUS / INT_ENABLE bits */
#define MPU_INT_DATA_RDY        0x01U
#define MPU_INT_FIFO_OFLOW      0x10U
#define MPU_INT_MOT             0x40U

typedef struct {
    uint8_t addr;        /* 7-bit I2C address                          */
    uint8_t who_am_i;    /* 0x68 = MPU6050, 0x70 = MPU6500, 0x71 = MPU9250 */
    uint8_t is_6500;     /* 1 = MPU6500 family (many "GY-521" boards)   */
    uint8_t accel_fs;    /* 0..3 -> +-2 / 4 / 8 / 16 g                  */
    uint8_t gyro_fs;     /* 0..3 -> +-250 / 500 / 1000 / 2000 deg/s     */
    uint8_t dlpf;        /* 0..6 digital low pass filter setting        */
    uint8_t smplrt_div;  /* sample rate = gyro rate / (1 + smplrt_div)  */
} mpu6050_t;

typedef struct {
    int16_t accel[3];
    int16_t temp;
    int16_t gyro[3];
} mpu6050_sample_t;

typedef struct {
    int32_t accel_response[3];  /* self-test enabled minus disabled, +-8 g    */
    int32_t gyro_response[3];   /* self-test enabled minus disabled, +-250 dps */
    uint8_t factory_trim[4];    /* raw SELF_TEST_X/Y/Z/A registers           */
} mpu6050_selftest_t;

int mpu6050_init(mpu6050_t *dev);
int mpu6050_apply_config(const mpu6050_t *dev);
int mpu6050_read_sample(const mpu6050_t *dev, mpu6050_sample_t *s);
int mpu6050_read_regs(const mpu6050_t *dev, uint8_t reg, uint8_t *buf, uint32_t len);
int mpu6050_write_reg(const mpu6050_t *dev, uint8_t reg, uint8_t value);
int mpu6050_self_test(const mpu6050_t *dev, mpu6050_selftest_t *out);

#endif /* MPU6050_H */
