/**
 * @file    imu_mahony.c
 * @brief   IMU 後端：Mahony 六軸互補濾波（APP_USE_MPU_DMP == 0 時編譯）。
 *
 * 演算法（教學重點）：
 *  - 陀螺儀積分姿態「短期準、長期漂」；加速度計提供重力方向
 *    「長期準、短期受動態加速度干擾」。
 *  - Mahony 濾波：用「量測重力 × 估測重力」的向量外積當誤差，
 *    以 PI 回授把陀螺積分往重力方向拉 —— 這就是互補濾波的本質。
 *  - 無磁力計 → yaw 沒有絕對參考，只能相對測量且緩慢漂移，
 *    這是硬體限制，DMP 亦然（要絕對航向需 HMC5883L 等磁力計）。
 *  - dt 用 DWT 實測而非假設 10ms：排程抖動不會變成角度誤差。
 */
#include "app/app_config.h"
#if (APP_USE_MPU_DMP == 0)

#include "drivers/imu.h"
#include "drivers/drv_mpu6050.h"
#include "bsp/bsp.h"
#include "stm32f1xx_hal.h"      /* HAL_Delay */
#include <math.h>

#define TWO_KP_NORMAL   1.0f     /* 2 * 比例增益 */
#define TWO_KP_BOOST    10.0f    /* 開機收斂加速期 */
#define TWO_KI          0.01f    /* 2 * 積分增益（吃殘餘零偏） */
#define BOOST_SAMPLES   300u     /* 前 3 秒（100Hz）用高增益快速對齊 */
#define DEG2RAD         0.0174532925f
#define RAD2DEG         57.2957795f

static imu_state_t s_state;
static bool    s_healthy;
static int16_t s_bias[3];
static float   s_yaw_ref;

/* 四元數姿態（w, x, y, z）與積分誤差項 */
static float q0 = 1.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;
static float integ_x, integ_y, integ_z;
static uint32_t s_last_us;

/* MPU INT（EXTI）計數：ISR context，只做遞增 */
static volatile uint32_t s_int_count;
static void mpu_int_isr(void)
{
    s_int_count++;
}

int imu_init(void)
{
    bsp_mpu_int_register(mpu_int_isr);

    /* 感測器初始化最多重試 3 次（上電時序/匯流排雜訊的常見對策） */
    int rc = APP_ENODEV;
    for (int i = 0; i < 3; i++) {
        rc = mpu6050_init();
        if (rc == APP_OK) {
            break;
        }
        HAL_Delay(50);
    }
    s_healthy = (rc == APP_OK);
    s_last_us = bsp_micros();
    return rc;
}

static float inv_sqrt(float x)
{
    return 1.0f / sqrtf(x);
}

static void mahony_update(float gx, float gy, float gz,
                          float ax, float ay, float az, float dt)
{
    float two_kp = (s_state.sample_count < BOOST_SAMPLES) ? TWO_KP_BOOST
                                                          : TWO_KP_NORMAL;

    float norm_sq = ax * ax + ay * ay + az * az;
    /* 加速度有效性檢查：合力遠離 1g（±40%）代表動態加速度大，
     * 此時只信陀螺儀，不做重力修正 */
    bool acc_valid = (norm_sq > 0.36f) && (norm_sq < 1.96f);

    if (acc_valid) {
        float recip = inv_sqrt(norm_sq);
        ax *= recip; ay *= recip; az *= recip;

        /* 估測重力方向（機體座標）＝旋轉矩陣第三列 */
        float vx = 2.0f * (q1 * q3 - q0 * q2);
        float vy = 2.0f * (q0 * q1 + q2 * q3);
        float vz = q0 * q0 - q1 * q1 - q2 * q2 + q3 * q3;

        /* 誤差 = 量測重力 × 估測重力（外積，正比於角度差） */
        float ex = ay * vz - az * vy;
        float ey = az * vx - ax * vz;
        float ez = ax * vy - ay * vx;

        integ_x += TWO_KI * ex * dt;
        integ_y += TWO_KI * ey * dt;
        integ_z += TWO_KI * ez * dt;

        gx += two_kp * ex + integ_x;
        gy += two_kp * ey + integ_y;
        gz += two_kp * ez + integ_z;
    }

    /* 四元數微分方程積分：q̇ = ½ q ⊗ ω */
    float half_dt = 0.5f * dt;
    float qa = q0, qb = q1, qc = q2;
    q0 += (-qb * gx - qc * gy - q3 * gz) * half_dt;
    q1 += (qa * gx + qc * gz - q3 * gy) * half_dt;
    q2 += (qa * gy - qb * gz + q3 * gx) * half_dt;
    q3 += (qa * gz + qb * gy - qc * gx) * half_dt;

    float recip = inv_sqrt(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    q0 *= recip; q1 *= recip; q2 *= recip; q3 *= recip;
}

static float wrap180(float deg)
{
    while (deg > 180.0f)  { deg -= 360.0f; }
    while (deg < -180.0f) { deg += 360.0f; }
    return deg;
}

int imu_update(void)
{
    if (!s_healthy) {
        return APP_ENODEV;
    }

    mpu6050_raw_t raw;
    int rc = mpu6050_read_raw(&raw);
    if (rc != APP_OK) {
        s_state.error_count++;
        /* 連續讀取失敗 → 標記 degraded，task_sys 會回報並嘗試復原 */
        if (s_state.error_count > 50u && (s_state.error_count % 100u) == 0u) {
            s_healthy = (mpu6050_init() == APP_OK);
        }
        return rc;
    }

    uint32_t now_us = bsp_micros();
    float dt = (float)(uint32_t)(now_us - s_last_us) * 1e-6f;
    s_last_us = now_us;
    dt = APP_CLAMP(dt, 0.001f, 0.05f);   /* 防護：異常 dt 不得毀掉姿態 */

    float gx = (float)(raw.gx - s_bias[0]) / MPU6050_GYRO_LSB_PER_DPS * DEG2RAD;
    float gy = (float)(raw.gy - s_bias[1]) / MPU6050_GYRO_LSB_PER_DPS * DEG2RAD;
    float gz = (float)(raw.gz - s_bias[2]) / MPU6050_GYRO_LSB_PER_DPS * DEG2RAD;
    float ax = (float)raw.ax / MPU6050_ACCEL_LSB_PER_G;
    float ay = (float)raw.ay / MPU6050_ACCEL_LSB_PER_G;
    float az = (float)raw.az / MPU6050_ACCEL_LSB_PER_G;

    mahony_update(gx, gy, gz, ax, ay, az, dt);

    /* 四元數 → 歐拉角（ZYX 航太慣例） */
    s_state.roll_deg = atan2f(2.0f * (q0 * q1 + q2 * q3),
                              1.0f - 2.0f * (q1 * q1 + q2 * q2)) * RAD2DEG;
    float sinp = 2.0f * (q0 * q2 - q3 * q1);
    sinp = APP_CLAMP(sinp, -1.0f, 1.0f);
    s_state.pitch_deg = asinf(sinp) * RAD2DEG;
    float yaw = atan2f(2.0f * (q0 * q3 + q1 * q2),
                       1.0f - 2.0f * (q2 * q2 + q3 * q3)) * RAD2DEG;
    s_state.yaw_deg = wrap180(yaw - s_yaw_ref);

    s_state.temp_cdegc = mpu6050_temp_cdegc(raw.temp_raw);
    s_state.sample_count++;
    return 1;
}

const imu_state_t *imu_get(void)
{
    return &s_state;
}

bool imu_healthy(void)
{
    return s_healthy;
}

void imu_set_gyro_bias(const int16_t bias[3])
{
    for (int i = 0; i < 3; i++) {
        s_bias[i] = bias[i];
    }
}

int imu_calibrate_gyro(int16_t bias_out[3])
{
    int16_t bias[3];
    int rc = mpu6050_calib_gyro(bias);
    if (rc != APP_OK) {
        return rc;
    }
    imu_set_gyro_bias(bias);
    integ_x = integ_y = integ_z = 0.0f;   /* 舊零偏吃進積分項，一併清除 */
    if (bias_out != NULL) {
        for (int i = 0; i < 3; i++) {
            bias_out[i] = bias[i];
        }
    }
    return APP_OK;
}

void imu_reset_yaw(void)
{
    /* 把「目前絕對 yaw」設為新的參考零點 */
    s_yaw_ref = wrap180(s_state.yaw_deg + s_yaw_ref);
    s_state.yaw_deg = 0.0f;
}

uint32_t imu_int_count(void)
{
    return s_int_count;
}

#endif /* APP_USE_MPU_DMP == 0 */
