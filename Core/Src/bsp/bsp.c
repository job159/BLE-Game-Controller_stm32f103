/**
 * @file    bsp.c
 * @brief   BSP 共用服務實作：DWT 微秒時基、重置原因、EXTI 分派。
 *
 * 教學重點：
 *  - Cortex-M3 的 DWT->CYCCNT 是免費的高解析度時基，不佔用任何 TIM。
 *  - 重置原因（RCC->CSR）是量產除錯的第一線索：開機 banner 印出、
 *    也透過 BLE STAT 封包回報，遠端就能判斷裝置是否曾看門狗重啟。
 */
#include "bsp/bsp.h"
#include "bsp/bsp_board.h"
#include "bsp/bsp_uart.h"

static uint32_t s_reset_cause;
static uint32_t s_cyc_per_us = 72u;   /* bsp_init() 依 SystemCoreClock 更新 */

/* MPU6050 INT（EXTI）通知的訂閱者：由 imu 模組註冊。
 * 用函式指標解耦 BSP 與驅動層，BSP 不需要 include 驅動標頭。 */
static void (*s_mpu_int_cb)(void);

void bsp_init(void)
{
    /* 1) 啟用 DWT 週期計數器（部分除錯器會先開，這裡確保無條件可用） */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    s_cyc_per_us = SystemCoreClock / 1000000u;
    if (s_cyc_per_us == 0u) {
        s_cyc_per_us = 1u;
    }

    /* 2) 擷取重置原因後清除旗標（旗標會跨重置保留，讀完必須清） */
    s_reset_cause = 0u;
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))  { s_reset_cause |= BSP_RST_POWER_ON; }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))  { s_reset_cause |= BSP_RST_NRST_PIN; }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))  { s_reset_cause |= BSP_RST_SOFTWARE; }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) { s_reset_cause |= BSP_RST_IWDG; }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST)) { s_reset_cause |= BSP_RST_WWDG; }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST)) { s_reset_cause |= BSP_RST_LOW_POWER; }
    __HAL_RCC_CLEAR_RESET_FLAGS();
}

uint32_t bsp_reset_cause(void)
{
    return s_reset_cause;
}

const char *bsp_reset_cause_str(void)
{
    /* 依嚴重性排序回報主要原因（上電重置常伴隨 PIN 旗標，POR 優先） */
    if (s_reset_cause & BSP_RST_IWDG)      { return "IWDG"; }
    if (s_reset_cause & BSP_RST_WWDG)      { return "WWDG"; }
    if (s_reset_cause & BSP_RST_SOFTWARE)  { return "SOFT"; }
    if (s_reset_cause & BSP_RST_POWER_ON)  { return "POR"; }
    if (s_reset_cause & BSP_RST_LOW_POWER) { return "LPWR"; }
    if (s_reset_cause & BSP_RST_NRST_PIN)  { return "PIN"; }
    return "UNKNOWN";
}

uint32_t bsp_micros(void)
{
    return DWT->CYCCNT / s_cyc_per_us;
}

void bsp_delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * s_cyc_per_us;
    /* 無號減法對計數器環繞（~59.6 秒 @72MHz）天然安全 */
    while ((DWT->CYCCNT - start) < ticks) {
        __NOP();
    }
}

void bsp_system_reset(void)
{
    dbg_flush();
    HAL_Delay(10);
    NVIC_SystemReset();
    for (;;) { }   /* noreturn 保證 */
}

void bsp_mpu_int_register(void (*cb)(void))
{
    s_mpu_int_cb = cb;
}

/**
 * @brief HAL EXTI 統一回呼（ISR context）。
 *        目前僅 MPU6050 INT 使用；新增 EXTI 來源時在此分派。
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == MPU_INT_Pin) {
        if (s_mpu_int_cb != NULL) {
            s_mpu_int_cb();
        }
    }
}
