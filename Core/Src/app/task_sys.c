/**
 * @file    task_sys.c
 * @brief   系統任務（1Hz）：狀態回報、看門狗總檢查；
 *          與儲存輪詢任務（250ms）。
 */
#include "app/app_tasks.h"
#include "app/app_config.h"
#include "bsp/bsp_uart.h"
#include "bsp/bsp_i2c.h"
#include "bsp/bsp_wdg.h"
#include "mw/sched.h"
#include "mw/storage.h"
#include "mw/proto.h"
#include "drivers/imu.h"
#include "drivers/drv_ssd1306.h"
#include "stm32f1xx_hal.h"

void task_storage(void)
{
    stor_poll();
}

/* 錯誤總量（飽和加總，供 STAT 封包粗略觀測；細項看 CLI stat） */
static uint16_t error_total(void)
{
    uint32_t total = 0u;
    total += bsp_i2c_stats(BSP_I2C_SENSOR)->xfer_err;
    total += bsp_i2c_stats(BSP_I2C_DISP)->xfer_err;
    total += dbg_stats()->hw_errors;
    total += ble_stats()->hw_errors;
    total += comm_proto_stats()->crc_errors;
    total += imu_get()->error_count;
    return (total > 0xFFFFu) ? 0xFFFFu : (uint16_t)total;
}

void task_sys(void)
{
    /* --- 1. 組裝並發送 STAT（bridge 模式下停發） --- */
    if (!app_state()->bridge_mode) {
        proto_sysstat_t st = {
            .cpu_percent = sched_cpu_percent(),
            .sys_flags   = (uint8_t)((imu_healthy()  ? 0x01u : 0u) |
                                     (oled_ok()      ? 0x02u : 0u) |
                                     (stor_healthy() ? 0x04u : 0u) |
                                     (app_state()->bridge_mode ? 0x08u : 0u)),
            .err_count   = error_total(),
            .boot_count  = stor_get()->boot_count,
            .uptime_s    = HAL_GetTick() / 1000u,
        };
        comm_send(PROTO_T_SYSSTAT, &st, sizeof(st));
    }

    /* --- 2. 看門狗總檢查：本任務簽到後檢查全體 --- */
    bsp_wdg_checkin(WDG_TASK_SYS);
    if (!bsp_wdg_service()) {
        /* 有任務未簽到：即將被 IWDG 重置。盡力留下現場線索 */
        dbg_printf("[sys ] WDG: task starvation detected!\r\n");
    }
}
