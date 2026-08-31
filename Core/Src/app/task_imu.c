/**
 * @file    task_imu.c
 * @brief   姿態任務：100Hz 解算 + 非同步校正請求處理。
 *
 * 校正流程展示「命令-非同步執行-事件回報」模式：
 *   BLE/CLI/按鍵 → 設 cal_request → 本任務下一輪執行（阻塞 1 秒）
 *   → 結果經 EVENT 封包 + OLED 提示回報。
 * 命令通道立即 ACK「已受理」，執行結果另行通知 —— 長操作的標準解法。
 */
#include "app/app_tasks.h"
#include "app/app_config.h"
#include "bsp/bsp_wdg.h"
#include "bsp/bsp_uart.h"
#include "mw/storage.h"
#include "drivers/imu.h"

void task_imu(void)
{
    app_state_t *app = app_state();

    if (app->cal_request) {
        app->cal_request = false;
        ui_notify("CAL: HOLD STILL");
        dbg_printf("[imu ] gyro calibration start (keep device still)\r\n");

        int16_t bias[3];
        int rc = imu_calibrate_gyro(bias);
        if (rc == APP_OK) {
            stor_record_t *rec = stor_get();
            rec->gyro_bias[0] = bias[0];
            rec->gyro_bias[1] = bias[1];
            rec->gyro_bias[2] = bias[2];
            rec->flags |= STOR_FLAG_GYRO_CAL;
            stor_mark_dirty();
            (void)stor_commit_now();   /* 校正值珍貴，立即落盤 */
            dbg_printf("[imu ] cal ok: bias=(%d,%d,%d)\r\n",
                       bias[0], bias[1], bias[2]);
            ui_notify("CAL OK");
            comm_send_event(PROTO_EV_CAL_DONE, 0u);
        } else {
            dbg_printf("[imu ] cal FAILED (%d) - device moved?\r\n", rc);
            ui_notify("CAL FAILED");
            comm_send_event(PROTO_EV_CAL_DONE, 1u);
        }
    }

    (void)imu_update();
    bsp_wdg_checkin(WDG_TASK_IMU);
}
