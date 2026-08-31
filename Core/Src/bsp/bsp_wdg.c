/**
 * @file    bsp_wdg.c
 * @brief   IWDG 任務簽到制實作。
 */
#include "bsp/bsp_wdg.h"
#include "bsp/bsp_board.h"

static volatile uint32_t s_checkin_bits;

void bsp_wdg_checkin(uint32_t task_bit)
{
    uint32_t key = app_enter_critical();
    s_checkin_bits |= task_bit;
    app_exit_critical(key);
}

bool bsp_wdg_service(void)
{
    uint32_t key = app_enter_critical();
    uint32_t bits = s_checkin_bits;
    bool all_ok = ((bits & WDG_TASK_ALL) == WDG_TASK_ALL);
    if (all_ok) {
        s_checkin_bits = 0u;   /* 收齊才歸零，下一輪重新收集 */
    }
    app_exit_critical(key);

    if (all_ok) {
        HAL_IWDG_Refresh(&hiwdg);
    }
    return all_ok;
}

void bsp_wdg_feed_raw(void)
{
    HAL_IWDG_Refresh(&hiwdg);
}
