/**
 * @file    bsp.h
 * @brief   BSP 共用服務：微秒時基（DWT）、重置原因、系統重啟。
 */
#ifndef BSP_H
#define BSP_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 重置原因（可多重旗標，例如上電 + NRST） */
typedef enum {
    BSP_RST_UNKNOWN = 0,
    BSP_RST_POWER_ON  = (1u << 0),  /* 上電 / 掉電重置 */
    BSP_RST_NRST_PIN  = (1u << 1),  /* 外部 NRST 腳位  */
    BSP_RST_SOFTWARE  = (1u << 2),  /* NVIC_SystemReset */
    BSP_RST_IWDG      = (1u << 3),  /* 獨立看門狗       */
    BSP_RST_WWDG      = (1u << 4),  /* 視窗看門狗       */
    BSP_RST_LOW_POWER = (1u << 5),
} bsp_reset_cause_t;

/**
 * @brief 基礎服務初始化：啟用 DWT 週期計數器、擷取並清除重置旗標。
 * @note  必須在讀取 bsp_reset_cause() 前呼叫（於 app_main_init 開頭）。
 */
void bsp_init(void);

/** @return 開機當下擷取的重置原因旗標組合 */
uint32_t bsp_reset_cause(void);

/** @return 重置原因的簡短字串（如 "IWDG"、"POR"、"SOFT+PIN"） */
const char *bsp_reset_cause_str(void);

/** @return DWT 週期計數器換算的微秒（32-bit 環繞，差值運算安全） */
uint32_t bsp_micros(void);

/** @brief 忙等 us 微秒（僅供驅動短延遲使用，任務中請勿長時間忙等） */
void bsp_delay_us(uint32_t us);

/** @brief 送出未送完的除錯訊息後執行軟體重置（不返回） */
void bsp_system_reset(void) __attribute__((noreturn));

/**
 * @brief 註冊 MPU6050 INT（EXTI 下降緣）的通知回呼。
 * @note  回呼於 ISR context 執行，僅能做旗標/計數等極短操作。
 */
void bsp_mpu_int_register(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif /* BSP_H */
