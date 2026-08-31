/**
 * @file    sched.h
 * @brief   協作式時間片排程器（bare-metal，無 RTOS）。
 *
 * 為什麼不用 RTOS（教學重點）？
 *  - F103C8 只有 64KB flash / 20KB RAM，本案工作負載全部是
 *    「短小的週期性任務」，協作式排程行為完全可預測、除錯簡單。
 *  - 代價：任何任務都不可長時間阻塞（>數 ms），否則拖累全系統
 *    —— 排程器內建逾時（overrun）統計，違規立刻可見。
 *  - 架構上任務函式簽名與 RTOS thread 相容，未來要換 FreeRTOS
 *    只需改排程外殼（見 Docs/architecture.md 移植章節）。
 *
 * 特性：
 *  - 靜態任務表（無動態記憶體）、相位（phase）錯開避免同拍尖峰。
 *  - 以 DWT 量測每次執行時間 → worst-case / CPU% 統計。
 */
#ifndef MW_SCHED_H
#define MW_SCHED_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_MAX_TASKS 10

typedef void (*sched_fn_t)(void);

typedef struct {
    const char *name;
    uint32_t    period_ms;    /* 0 = 停用 */
    uint32_t    runs;
    uint32_t    overruns;     /* 單次執行時間 > 週期 的次數 */
    uint32_t    last_us;      /* 最近一次執行耗時 */
    uint32_t    worst_us;     /* 歷史最大耗時 */
} sched_task_info_t;

/**
 * @brief 註冊任務。
 * @param period_ms 執行週期；@param phase_ms 首次執行的相位偏移
 * @return 任務 id（>=0），表滿回傳 APP_ENOSPACE
 */
int sched_add(const char *name, sched_fn_t fn, uint32_t period_ms,
              uint32_t phase_ms);

/** @brief 跑一輪：執行所有到期任務（由主迴圈反覆呼叫） */
void sched_run(void);

/** @brief 動態調整週期（0 = 停用該任務）。用於遙測頻率指令 */
int sched_set_period(int id, uint32_t period_ms);

int sched_task_count(void);
const sched_task_info_t *sched_task_info(int id);

/** @return 最近一個統計窗（1 秒）的 CPU 使用率 0..100 */
uint8_t sched_cpu_percent(void);

#ifdef __cplusplus
}
#endif

#endif /* MW_SCHED_H */
