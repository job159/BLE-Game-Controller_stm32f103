/**
 * @file    sched.c
 * @brief   協作式排程器實作。
 *
 * 時間處理原則：所有比較都用「無號差值」（now - last >= period），
 * 對 HAL_GetTick 的 32-bit 環繞（49.7 天）天然安全。
 */
#include "mw/sched.h"
#include "bsp/bsp.h"
#include "stm32f1xx_hal.h"

typedef struct {
    sched_task_info_t info;
    sched_fn_t fn;
    uint32_t   next_ms;     /* 下次到期時刻 */
} task_t;

static task_t  s_tasks[SCHED_MAX_TASKS];
static int     s_count;

/* CPU 使用率統計：1 秒窗口內累計「任務執行中」的微秒數 */
static uint32_t s_busy_us;
static uint32_t s_win_start_ms;
static uint8_t  s_cpu_pct;

int sched_add(const char *name, sched_fn_t fn, uint32_t period_ms,
              uint32_t phase_ms)
{
    if ((fn == NULL) || (name == NULL)) {
        return APP_EINVAL;
    }
    if (s_count >= SCHED_MAX_TASKS) {
        return APP_ENOSPACE;
    }
    task_t *t = &s_tasks[s_count];
    t->info.name = name;
    t->info.period_ms = period_ms;
    t->fn = fn;
    t->next_ms = HAL_GetTick() + phase_ms;
    return s_count++;
}

int sched_set_period(int id, uint32_t period_ms)
{
    if ((id < 0) || (id >= s_count)) {
        return APP_EINVAL;
    }
    s_tasks[id].info.period_ms = period_ms;
    if (period_ms > 0u) {
        s_tasks[id].next_ms = HAL_GetTick() + period_ms;
    }
    return APP_OK;
}

void sched_run(void)
{
    uint32_t now = HAL_GetTick();

    for (int i = 0; i < s_count; i++) {
        task_t *t = &s_tasks[i];
        if (t->info.period_ms == 0u) {
            continue;
        }
        if ((int32_t)(now - t->next_ms) < 0) {
            continue;   /* 尚未到期 */
        }

        uint32_t t0 = bsp_micros();
        t->fn();
        uint32_t dt = bsp_micros() - t0;

        t->info.runs++;
        t->info.last_us = dt;
        if (dt > t->info.worst_us) {
            t->info.worst_us = dt;
        }
        if (dt > (t->info.period_ms * 1000u)) {
            t->info.overruns++;
        }
        s_busy_us += dt;

        /* 以「排定時刻 + 週期」推進而非「現在 + 週期」，
         * 避免執行耗時造成的頻率漂移；若已落後太多（>3 週期）
         * 則直接重新對齊，防止補跑風暴。 */
        t->next_ms += t->info.period_ms;
        now = HAL_GetTick();
        if ((int32_t)(now - t->next_ms) > (int32_t)(3u * t->info.period_ms)) {
            t->next_ms = now + t->info.period_ms;
        }
    }

    /* 1 秒統計窗 */
    if ((uint32_t)(now - s_win_start_ms) >= 1000u) {
        uint32_t win_us = (uint32_t)(now - s_win_start_ms) * 1000u;
        uint32_t pct = (win_us > 0u) ? ((uint64_t)s_busy_us * 100u / win_us) : 0u;
        s_cpu_pct = (uint8_t)APP_MIN(pct, 100u);
        s_busy_us = 0u;
        s_win_start_ms = now;
    }
}

int sched_task_count(void)
{
    return s_count;
}

const sched_task_info_t *sched_task_info(int id)
{
    if ((id < 0) || (id >= s_count)) {
        return NULL;
    }
    return &s_tasks[id].info;
}

uint8_t sched_cpu_percent(void)
{
    return s_cpu_pct;
}
