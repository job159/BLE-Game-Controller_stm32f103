/**
 * @file    drv_mpu6050.h
 * @brief   MPU6050 暫存器層驅動（不含姿態解算）。
 *
 * 量程設定（drv_mpu6050.c 內固定，變更需同步換算係數）：
 *   陀螺儀 ±2000 dps → 16.4  LSB/(°/s)
 *   加速度 ±4 g      → 8192  LSB/g
 *   DLPF 42Hz、取樣 100Hz
 */
#ifndef DRV_MPU6050_H
#define DRV_MPU6050_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MPU6050_GYRO_LSB_PER_DPS   16.4f
#define MPU6050_ACCEL_LSB_PER_G    8192.0f

typedef struct {
    int16_t ax, ay, az;
    int16_t temp_raw;
    int16_t gx, gy, gz;
} mpu6050_raw_t;

/** @brief 上電初始化（含 WHO_AM_I 驗證與量程配置） */
int mpu6050_init(void);

int mpu6050_probe(void);

/** @brief 一次 burst 讀取 14 bytes（accel + temp + gyro） */
int mpu6050_read_raw(mpu6050_raw_t *out);

/** @brief raw 溫度 → 攝氏 ×100 */
int16_t mpu6050_temp_cdegc(int16_t temp_raw);

/**
 * @brief 靜置校正陀螺儀零偏（阻塞約 1 秒；期間裝置必須完全靜止）。
 * @param[out] bias 每軸平均零偏（raw LSB）
 * @return APP_OK；偵測到晃動回傳 APP_ERR
 */
int mpu6050_calib_gyro(int16_t bias[3]);

#ifdef __cplusplus
}
#endif

#endif /* DRV_MPU6050_H */
