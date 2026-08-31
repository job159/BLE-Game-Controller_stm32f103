/**
 * @file    app_main.c
 * @brief   應用初始化順序與主迴圈。
 *
 * 初始化順序是刻意設計的（依賴由下而上，失敗可降級）：
 *   BSP 基礎 → UART（先能講話）→ 儲存（恢復參數）
 *   → IMU（吃儲存的校正值）→ OLED → 通訊 → CLI → 任務註冊
 *
 * 「降級運行」原則：任何單一周邊故障（IMU 拔線、EEPROM 損毀、
 * OLED 缺席）都不會讓系統當機 —— 記錄狀態、繼續服務其餘功能，
 * 並透過 LED / STAT 封包 / CLI 讓故障可觀測。這是量產韌體
 * 與課堂範例最大的差別。
 */
#include "app/app_main.h"
#include "app/app_config.h"
#include "app/app_tasks.h"
#include "bsp/bsp.h"
#include "bsp/bsp_uart.h"
#include "bsp/bsp_wdg.h"
#include "mw/sched.h"
#include "mw/storage.h"
#include "mw/cli.h"
#include "drivers/imu.h"
#include "drivers/drv_ssd1306.h"
#include "stm32f1xx_hal.h"      /* SystemCoreClock */

static app_state_t s_app;
int g_task_id_comm_tx = -1;

app_state_t *app_state(void)
{
    return &s_app;
}

static void banner(void)
{
    dbg_printf("\r\n=================================================\r\n");
    dbg_printf(" %s v%s (proto v%d)\r\n",
               APP_FW_NAME, APP_FW_VERSION, APP_PROTO_VERSION);
    dbg_printf(" build : %s %s\r\n", __DATE__, __TIME__);
    dbg_printf(" core  : STM32F103 @ %lu MHz\r\n",
               (unsigned long)(SystemCoreClock / 1000000u));
    dbg_printf(" reset : %s (0x%02lx)\r\n",
               bsp_reset_cause_str(), (unsigned long)bsp_reset_cause());
    dbg_printf("=================================================\r\n");
}

void app_main_init(void)
{
    /* --- 1. 基礎服務 --- */
    bsp_init();
    bsp_uart_init();
    banner();

    /* --- 2. 參數儲存 --- */
    int rc = stor_init();
    if (rc == APP_OK) {
        dbg_printf("[stor] record loaded (slot %lu, boots=%lu)\r\n",
                   (unsigned long)stor_stats()->load_source,
                   (unsigned long)stor_get()->boot_count);
    } else if (rc == APP_ENODEV) {
        dbg_printf("[stor] EEPROM offline! running with defaults\r\n");
    } else {
        dbg_printf("[stor] no valid record, defaults applied\r\n");
    }
    stor_get()->boot_count++;
    (void)stor_commit_now();   /* 開機次數立即落盤（EEPROM 離線時安全失敗） */

    /* --- 3. IMU（先恢復校正值再初始化） --- */
    if (stor_get()->flags & STOR_FLAG_GYRO_CAL) {
        imu_set_gyro_bias(stor_get()->gyro_bias);
        dbg_printf("[imu ] gyro bias restored (%d,%d,%d)\r\n",
                   stor_get()->gyro_bias[0], stor_get()->gyro_bias[1],
                   stor_get()->gyro_bias[2]);
    }
    rc = imu_init();
    dbg_printf("[imu ] init %s (backend: %s)\r\n",
               (rc == APP_OK) ? "ok" : "FAILED - degraded mode",
               (APP_USE_MPU_DMP != 0) ? "DMP" : "Mahony");

    /* --- 4. 顯示 --- */
#if APP_USE_OLED
    rc = oled_init();
    dbg_printf("[oled] init %s\r\n", (rc == APP_OK) ? "ok" : "FAILED - no display");
#endif
    task_ui_init();

    /* --- 5. 通訊與按鍵 --- */
    task_comm_init();
    task_btn_init();
#if APP_USE_CLI
    app_cli_init();
#endif

    /* --- 6. 任務註冊（相位錯開，避免同一 tick 全員擠兌） --- */
    (void)sched_add("imu",     task_imu,     APP_IMU_PERIOD_MS,     0u);
    (void)sched_add("btn",     task_btn,     APP_BTN_PERIOD_MS,     3u);
    (void)sched_add("comm_rx", task_comm_rx, APP_COMM_RX_PERIOD_MS, 5u);
    uint8_t hz = stor_get()->telemetry_hz;
    if (hz > APP_TELEMETRY_HZ_MAX) {
        hz = APP_TELEMETRY_HZ_DEF;   /* 防禦：儲存值可能來自異常記錄 */
    }
    g_task_id_comm_tx = sched_add("comm_tx", task_comm_tx,
                                  (hz > 0u) ? (1000u / hz) : 0u, 7u);
    (void)sched_add("ui",      task_ui,      APP_UI_PERIOD_MS,      20u);
    (void)sched_add("storage", task_storage, 250u,                  40u);
    (void)sched_add("sys",     task_sys,     1000u,                 60u);

    dbg_printf("[sys ] %d tasks registered, telemetry %u Hz\r\n",
               sched_task_count(), (unsigned)hz);
    dbg_printf("[sys ] system up\r\n");
}

void app_main_loop(void)
{
    sched_run();
    oled_poll();   /* OLED 分頁刷新接力（無刷新進行時零成本） */
}
