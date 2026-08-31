/**
 * @file    imu_dmp.c
 * @brief   IMU 後端：InvenSense eMPL DMP（APP_USE_MPU_DMP == 1 時編譯）。
 *
 * DMP（Digital Motion Processor）是 MPU6050 內建的協處理器：
 * 韌體開機時把 ~3KB 的 DMP firmware 灌進感測器，之後六軸融合
 * 在感測器內完成，MCU 只需從 FIFO 讀出現成的四元數 —— 以 I2C
 * 頻寬換 CPU 算力，是低階 MCU 常見的取捨。
 *
 * 注意：eMPL 檔案需自行放入 Drivers/eMPL/（見 Docs/dmp_porting.md）。
 * 若使用「正點原子等改版 inv_mpu.c」且 mpu_init 簽名為無參數，
 * 請把下方 mpu_init(&int_param) 改成 mpu_init()。
 */
#include "app/app_config.h"
#if (APP_USE_MPU_DMP == 1)

#include "drivers/imu.h"
#include "drivers/drv_mpu6050.h"   /* 溫度換算與量程常數共用 */
#include "bsp/bsp.h"
#include "bsp/bsp_uart.h"
#include "stm32f1xx_hal.h"
#include <math.h>

/* empl_port.h 必須先於 inv_mpu.h（決定 struct int_param_s 形狀） */
#include "drivers/empl_port.h"

/* eMPL 標頭位於 Drivers/eMPL/（CubeIDE 對 Drivers 整目錄編譯，
 * 但 include path 未涵蓋 —— 用相對路徑保持零設定即可建置；
 * 亦可改在 IDE 加入 include path 後寫成 #include "inv_mpu.h"） */
#include "../../../Drivers/eMPL/inv_mpu.h"
#include "../../../Drivers/eMPL/inv_mpu_dmp_motion_driver.h"

#define DMP_FIFO_RATE_HZ   100
#define Q30                1073741824.0f
#define RAD2DEG            57.2957795f

static imu_state_t s_state;
static bool  s_healthy;
static float s_yaw_ref;

static volatile uint32_t s_int_count;
static volatile bool s_data_ready;

static void mpu_int_isr(void)
{
    s_int_count++;
    s_data_ready = true;
}

/* 感測器安裝方向矩陣（晶片軸 → 機體軸）；水平正放為單位矩陣 */
static const signed char s_orient_mtx[9] = {
    1, 0, 0,
    0, 1, 0,
    0, 0, 1,
};

static unsigned short inv_row_2_scale(const signed char *row)
{
    unsigned short b;
    if (row[0] > 0)       { b = 0; }
    else if (row[0] < 0)  { b = 4; }
    else if (row[1] > 0)  { b = 1; }
    else if (row[1] < 0)  { b = 5; }
    else if (row[2] > 0)  { b = 2; }
    else if (row[2] < 0)  { b = 6; }
    else                  { b = 7; }
    return b;
}

static unsigned short orient_mtx_to_scalar(const signed char *mtx)
{
    unsigned short scalar;
    scalar  = inv_row_2_scale(mtx);
    scalar |= inv_row_2_scale(mtx + 3) << 3;
    scalar |= inv_row_2_scale(mtx + 6) << 6;
    return scalar;
}

int imu_init(void)
{
    bsp_mpu_int_register(mpu_int_isr);
    s_healthy = false;

    struct int_param_s int_param = {0};
    if (mpu_init(&int_param) != 0) {
        dbg_printf("[imu] mpu_init fail\r\n");
        return APP_ENODEV;
    }
    if (mpu_set_sensors(INV_XYZ_GYRO | INV_XYZ_ACCEL) != 0) {
        return APP_ERR;
    }
    if (mpu_configure_fifo(INV_XYZ_GYRO | INV_XYZ_ACCEL) != 0) {
        return APP_ERR;
    }
    if (mpu_set_sample_rate(DMP_FIFO_RATE_HZ) != 0) {
        return APP_ERR;
    }
    if (dmp_load_motion_driver_firmware() != 0) {
        dbg_printf("[imu] DMP firmware load fail\r\n");
        return APP_ERR;
    }
    if (dmp_set_orientation(orient_mtx_to_scalar(s_orient_mtx)) != 0) {
        return APP_ERR;
    }
    if (dmp_enable_feature(DMP_FEATURE_6X_LP_QUAT | DMP_FEATURE_SEND_RAW_ACCEL |
                           DMP_FEATURE_SEND_CAL_GYRO | DMP_FEATURE_GYRO_CAL) != 0) {
        return APP_ERR;
    }
    if (dmp_set_fifo_rate(DMP_FIFO_RATE_HZ) != 0) {
        return APP_ERR;
    }
    if (mpu_set_dmp_state(1) != 0) {
        return APP_ERR;
    }

    s_healthy = true;
    return APP_OK;
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

    short gyro[3], accel[3], sensors;
    unsigned char more;
    long quat[4];
    unsigned long ts;

    if (dmp_read_fifo(gyro, accel, quat, &ts, &sensors, &more) != 0) {
        return 0;   /* FIFO 尚無完整封包屬正常（100Hz 對 100Hz 輪詢） */
    }
    if (!(sensors & INV_WXYZ_QUAT)) {
        return 0;
    }

    float q0 = (float)quat[0] / Q30;
    float q1 = (float)quat[1] / Q30;
    float q2 = (float)quat[2] / Q30;
    float q3 = (float)quat[3] / Q30;

    s_state.roll_deg = atan2f(2.0f * (q0 * q1 + q2 * q3),
                              1.0f - 2.0f * (q1 * q1 + q2 * q2)) * RAD2DEG;
    float sinp = 2.0f * (q0 * q2 - q3 * q1);
    sinp = APP_CLAMP(sinp, -1.0f, 1.0f);
    s_state.pitch_deg = asinf(sinp) * RAD2DEG;
    float yaw = atan2f(2.0f * (q0 * q3 + q1 * q2),
                       1.0f - 2.0f * (q2 * q2 + q3 * q3)) * RAD2DEG;
    s_state.yaw_deg = wrap180(yaw - s_yaw_ref);

    /* 溫度另走暫存器讀取（DMP FIFO 不含溫度），降頻讀取即可 */
    if ((s_state.sample_count % 100u) == 0u) {
        mpu6050_raw_t raw;
        if (mpu6050_read_raw(&raw) == APP_OK) {
            s_state.temp_cdegc = mpu6050_temp_cdegc(raw.temp_raw);
        }
    }

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
    /* DMP 模式：零偏由 DMP_FEATURE_GYRO_CAL 於裝置端自動校正
     * （靜置 8 秒自動歸零），儲存的 bias 不適用 */
    APP_UNUSED(bias);
}

int imu_calibrate_gyro(int16_t bias_out[3])
{
    /* 交由 DMP 自動校正；回報成功讓上層流程一致 */
    if (bias_out != NULL) {
        bias_out[0] = bias_out[1] = bias_out[2] = 0;
    }
    return APP_OK;
}

void imu_reset_yaw(void)
{
    s_yaw_ref = wrap180(s_state.yaw_deg + s_yaw_ref);
    s_state.yaw_deg = 0.0f;
}

uint32_t imu_int_count(void)
{
    return s_int_count;
}

#endif /* APP_USE_MPU_DMP == 1 */
