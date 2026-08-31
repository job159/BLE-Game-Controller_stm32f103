/**
 * @file    bsp_wdg.h
 * @brief   獨立看門狗（IWDG）服務。
 *
 * 策略（見 Docs/architecture.md）：
 *  - IWDG 由 CubeMX 於開機即啟動（約 4 秒逾時），之後無法停止 —— 這是刻意的：
 *    看門狗保護的是「整個韌體生命週期」，包含初始化階段。
 *  - 應用層採「任務簽到制」：每個關鍵任務執行時呼叫 bsp_wdg_checkin(bit)，
 *    task_sys 每秒確認所有關鍵位元到齊才真正餵狗。
 *    任何一個任務卡死 → 停止餵狗 → 硬體重置 → 開機 banner 顯示 "IWDG"。
 */
#ifndef BSP_WDG_H
#define BSP_WDG_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 關鍵任務簽到位元 */
#define WDG_TASK_IMU    (1u << 0)
#define WDG_TASK_BTN    (1u << 1)
#define WDG_TASK_COMM   (1u << 2)
#define WDG_TASK_SYS    (1u << 3)
#define WDG_TASK_ALL    (WDG_TASK_IMU | WDG_TASK_BTN | WDG_TASK_COMM | WDG_TASK_SYS)

/** @brief 任務簽到（可在任何 context 呼叫，只做原子 OR） */
void bsp_wdg_checkin(uint32_t task_bit);

/**
 * @brief 檢查簽到狀況並決定是否餵狗（由 task_sys 每秒呼叫一次）。
 * @return true = 已餵狗；false = 有任務未簽到（即將重置，回傳值供記錄）
 */
bool bsp_wdg_service(void);

/**
 * @brief 直接餵狗。僅限「已知的長時間阻塞操作」內部使用
 *        （例如陀螺儀校正迴圈），一般任務嚴禁呼叫。
 */
void bsp_wdg_feed_raw(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_WDG_H */
