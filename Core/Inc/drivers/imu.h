/**
 * @file    imu.h
 * @brief   IMU 姿態服務介面（後端可抽換）。
 *
 * 兩個後端實作同一介面，由 app_config.h 的 APP_USE_MPU_DMP 選擇：
 *   imu_mahony.c  — 純軟體 Mahony 互補濾波（預設，開箱即用）
 *   imu_dmp.c     — InvenSense eMPL DMP 硬體解算（需外掛 eMPL 原始碼）
 *
 * 這就是「介面與實作分離」：上層（task_imu / task_ui / task_comm）
 * 只認得本介面，換後端零改動 —— 也是單元測試時 mock 的掛載點。
 */
#ifndef DRV_IMU_H
#define DRV_IMU_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    roll_deg;     /* 橫滾  [-180, 180] */
    float    pitch_deg;    /* 俯仰  [-90, 90]   */
    float    yaw_deg;      /* 偏航  [-180, 180]（無磁力計，會緩慢漂移） */
    int16_t  temp_cdegc;   /* 晶片溫度 ×100 */
    uint32_t sample_count;
    uint32_t error_count;
} imu_state_t;

/** @brief 初始化（含感測器組態）。失敗時系統以 degraded mode 繼續 */
int imu_init(void);

/**
 * @brief 週期呼叫（APP_IMU_PERIOD_MS）：取樣並更新姿態。
 * @return 1 = 有新姿態；0 = 本次無資料；負值 = 錯誤
 */
int imu_update(void);

const imu_state_t *imu_get(void);

bool imu_healthy(void);

/** @brief 設定陀螺儀零偏（開機時由 storage 恢復） */
void imu_set_gyro_bias(const int16_t bias[3]);

/**
 * @brief 現場校正陀螺儀零偏（阻塞約 1 秒，裝置須靜置）。
 * @param[out] bias_out 校正結果（呼叫者負責持久化）；可為 NULL
 */
int imu_calibrate_gyro(int16_t bias_out[3]);

/** @brief 將目前航向歸零（yaw 相對化） */
void imu_reset_yaw(void);

/** @brief MPU INT 腳（EXTI）觸發次數 —— 教學觀測用 */
uint32_t imu_int_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_IMU_H */
