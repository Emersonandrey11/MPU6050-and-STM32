/**
 * @file  telemetry.h
 * @brief RAM "mailbox" read by the PC through the ST-Link (SWD).
 *
 * The firmware keeps the latest MPU6050 data in one global struct
 * (g_telemetry). The PC tool tools/mpu6050_monitor.py finds it by symbol
 * name in the ELF file and reads it with OpenOCD while the MCU keeps running,
 * so no UART adapter is needed. The PC writes commands into the cmd_* fields.
 *
 * Consistency: the firmware increments seq_tail (last word) BEFORE changing
 * anything and copies it to seq_head (first words) AFTER. The PC reads the
 * block from low to high addresses and accepts it only if seq_head equals
 * seq_tail.
 *
 * Keep this layout in sync with TELEMETRY_FORMAT in tools/mpu6050_monitor.py.
 */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stddef.h>
#include <stdint.h>

#define TELEMETRY_MAGIC         0x3655504DUL   /* "MPU6" */
#define TELEMETRY_VERSION       1U

/* status bits */
#define TM_STATUS_MPU_OK        0x01U
#define TM_STATUS_SELFTEST_OK   0x02U
#define TM_STATUS_VIA_INT_PIN   0x04U   /* last sample was triggered by PB12 */

/* commands written by the PC */
#define TM_CMD_SET_ACCEL_FS     1U      /* arg 0..3 */
#define TM_CMD_SET_GYRO_FS      2U      /* arg 0..3 */
#define TM_CMD_SET_DLPF         3U      /* arg 0..6 */
#define TM_CMD_SET_SMPLRT_DIV   4U      /* arg 0..255 */
#define TM_CMD_SELF_TEST        5U
#define TM_CMD_REINIT           6U
#define TM_CMD_SET_AD0          7U      /* arg 0/1 -> address 0x68/0x69 */

typedef struct {
    /* ---- written by the firmware ---- */
    uint32_t magic;            /*   0 */
    uint16_t version;          /*   4 */
    uint16_t size;             /*   6 */
    uint32_t seq_head;         /*   8 */
    uint32_t uptime_ms;        /*  12 */
    uint32_t sample_count;     /*  16 */
    uint32_t sample_time_ms;   /*  20 */
    int16_t  accel[3];         /*  24  raw */
    int16_t  temp;             /*  30  raw */
    int16_t  gyro[3];          /*  32  raw */
    uint8_t  i2c_addr;         /*  38 */
    uint8_t  who_am_i;         /*  39 */
    uint32_t gyro_int[3];      /*  40  sum of raw * dt_ms (wraps) */
    uint8_t  status;           /*  52 */
    uint8_t  accel_fs;         /*  53 */
    uint8_t  gyro_fs;          /*  54 */
    uint8_t  dlpf;             /*  55 */
    uint8_t  smplrt_div;       /*  56 */
    uint8_t  int_status;       /*  57 */
    uint8_t  st_trim[4];       /*  58  SELF_TEST_X/Y/Z/A */
    uint8_t  ad0_level;        /*  62 */
    uint8_t  int_pin_level;    /*  63 */
    int32_t  st_accel[3];      /*  64  self-test response, +-8 g */
    int32_t  st_gyro[3];       /*  76  self-test response, +-250 dps */
    uint32_t motion_count;     /*  88 */
    uint32_t int_pin_count;    /*  92 */
    uint32_t i2c_errors;       /*  96 */
    uint32_t selftest_count;   /* 100 */
    uint8_t  regs[128];        /* 104  register dump, refreshed every second */
    /* ---- written by the PC ---- */
    uint32_t cmd_seq;          /* 232 */
    uint8_t  cmd_id;           /* 236 */
    uint8_t  cmd_arg;          /* 237 */
    uint16_t cmd_reserved;     /* 238 */
    /* ---- written by the firmware ---- */
    uint32_t cmd_ack;          /* 240 */
    uint32_t seq_tail;         /* 244 */
} telemetry_t;

_Static_assert(sizeof(telemetry_t) == 248U, "telemetry layout changed");
_Static_assert(offsetof(telemetry_t, regs) == 104U, "telemetry layout changed");
_Static_assert(offsetof(telemetry_t, cmd_seq) == 232U, "telemetry layout changed");

extern volatile telemetry_t g_telemetry;

#endif /* TELEMETRY_H */
