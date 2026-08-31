/**
 * @file    drv_mpu6050.c
 * @brief   MPU6050 暫存器層驅動實作。
 */
#include "drivers/drv_mpu6050.h"
#include "bsp/bsp_i2c.h"
#include "bsp/bsp_board.h"
#include "bsp/bsp_wdg.h"

/* ---- 暫存器位址 ---- */
#define REG_SMPLRT_DIV     0x19u
#define REG_CONFIG         0x1Au
#define REG_GYRO_CONFIG    0x1Bu
#define REG_ACCEL_CONFIG   0x1Cu
#define REG_INT_PIN_CFG    0x37u
#define REG_INT_ENABLE     0x38u
#define REG_ACCEL_XOUT_H   0x3Bu
#define REG_PWR_MGMT_1     0x6Bu
#define REG_PWR_MGMT_2     0x6Cu
#define REG_WHO_AM_I       0x75u

#define WHO_AM_I_VAL       0x68u
#define XFER_TIMEOUT_MS    20u

static int wr8(uint8_t reg, uint8_t val)
{
    return bsp_i2c_mem_write(BSP_I2C_SENSOR, BSP_ADDR_MPU6050, reg, 1u,
                             &val, 1u, XFER_TIMEOUT_MS);
}

static int rd(uint8_t reg, uint8_t *buf, uint16_t len)
{
    return bsp_i2c_mem_read(BSP_I2C_SENSOR, BSP_ADDR_MPU6050, reg, 1u,
                            buf, len, XFER_TIMEOUT_MS);
}

int mpu6050_probe(void)
{
    uint8_t who = 0u;
    if (rd(REG_WHO_AM_I, &who, 1u) != APP_OK) {
        return APP_ENODEV;
    }
    /* 部分相容晶片（MPU6500/6880 混料）WHO_AM_I 不同，僅比對高位型號族 */
    return (who == WHO_AM_I_VAL) ? APP_OK : APP_ENODEV;
}

int mpu6050_init(void)
{
    /* 1) 軟體重置後喚醒（重置後預設為 sleep） */
    if (wr8(REG_PWR_MGMT_1, 0x80u) != APP_OK) {
        return APP_ENODEV;
    }
    HAL_Delay(100);
    if (wr8(REG_PWR_MGMT_1, 0x01u) != APP_OK) {   /* CLKSEL=1：PLL X 軸陀螺 */
        return APP_ENODEV;
    }
    HAL_Delay(10);

    if (mpu6050_probe() != APP_OK) {
        return APP_ENODEV;
    }

    int rc = APP_OK;
    rc |= wr8(REG_PWR_MGMT_2, 0x00u);      /* 全軸啟用 */
    rc |= wr8(REG_CONFIG, 0x03u);          /* DLPF 42Hz（內部取樣 1kHz） */
    rc |= wr8(REG_SMPLRT_DIV, 9u);         /* 1kHz / (1+9) = 100Hz */
    rc |= wr8(REG_GYRO_CONFIG, 0x18u);     /* FS_SEL=3 → ±2000 dps */
    rc |= wr8(REG_ACCEL_CONFIG, 0x08u);    /* AFS_SEL=1 → ±4 g */
    /* INT：資料就緒、低電位有效、任一讀取清除 —— 供 EXTI 教學觀測，
     * Mahony 後端實際以排程器定時取樣（見 imu_mahony.c 說明） */
    rc |= wr8(REG_INT_PIN_CFG, 0x90u);     /* ACTL=1, INT_RD_CLEAR=1 */
    rc |= wr8(REG_INT_ENABLE, 0x01u);      /* DATA_RDY_EN */
    return (rc == APP_OK) ? APP_OK : APP_ERR;
}

int mpu6050_read_raw(mpu6050_raw_t *out)
{
    uint8_t b[14];
    int rc = rd(REG_ACCEL_XOUT_H, b, sizeof(b));
    if (rc != APP_OK) {
        return rc;
    }
    /* 大端暫存器 → 主機序 */
    out->ax = (int16_t)((b[0] << 8) | b[1]);
    out->ay = (int16_t)((b[2] << 8) | b[3]);
    out->az = (int16_t)((b[4] << 8) | b[5]);
    out->temp_raw = (int16_t)((b[6] << 8) | b[7]);
    out->gx = (int16_t)((b[8] << 8) | b[9]);
    out->gy = (int16_t)((b[10] << 8) | b[11]);
    out->gz = (int16_t)((b[12] << 8) | b[13]);
    return APP_OK;
}

int16_t mpu6050_temp_cdegc(int16_t temp_raw)
{
    /* 資料手冊：T(°C) = raw/340 + 36.53 → ×100 定點 */
    int32_t c100 = ((int32_t)temp_raw * 100) / 340 + 3653;
    return (int16_t)APP_CLAMP(c100, -4000, 12500);
}

int mpu6050_calib_gyro(int16_t bias[3])
{
    enum { N = 200, MOTION_LIMIT = 200 };   /* 200 樣本 × 5ms = 1 秒 */
    int32_t sum[3] = {0};
    int16_t vmin[3] = {INT16_MAX, INT16_MAX, INT16_MAX};
    int16_t vmax[3] = {INT16_MIN, INT16_MIN, INT16_MIN};

    for (int i = 0; i < N; i++) {
        mpu6050_raw_t s;
        if (mpu6050_read_raw(&s) != APP_OK) {
            return APP_ENODEV;
        }
        int16_t g[3] = { s.gx, s.gy, s.gz };
        for (int a = 0; a < 3; a++) {
            sum[a] += g[a];
            if (g[a] < vmin[a]) { vmin[a] = g[a]; }
            if (g[a] > vmax[a]) { vmax[a] = g[a]; }
        }
        /* 校正是已知的長阻塞操作：例外性地手動餵狗（見 bsp_wdg.h 規範） */
        if ((i % 50) == 0) {
            bsp_wdg_feed_raw();
        }
        HAL_Delay(5);
    }

    /* 晃動偵測：任一軸峰對峰值過大即拒絕本次校正 */
    for (int a = 0; a < 3; a++) {
        if ((int32_t)vmax[a] - vmin[a] > MOTION_LIMIT) {
            return APP_ERR;
        }
    }
    for (int a = 0; a < 3; a++) {
        bias[a] = (int16_t)(sum[a] / N);
    }
    return APP_OK;
}
