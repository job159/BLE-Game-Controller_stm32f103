/**
 * @file    app_common.h
 * @brief   全專案共用的回傳碼、巨集與臨界區工具。
 *
 * 設計原則：
 *  - 統一的錯誤碼讓每一層 API 行為一致（0 成功、負值失敗），
 *    上層不需要認識 HAL_StatusTypeDef，是「可移植性」的第一道防線。
 *  - 臨界區使用 PRIMASK 保存/還原，可安全巢狀呼叫，
 *    僅用於保護「主迴圈與 ISR 共享」的極短存取。
 */
#ifndef APP_COMMON_H
#define APP_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 統一回傳碼（0 = 成功，負值 = 失敗類別） ---- */
enum {
    APP_OK        = 0,
    APP_ERR       = -1,   /* 一般性失敗          */
    APP_ETIMEOUT  = -2,   /* 逾時                */
    APP_EBUSY     = -3,   /* 資源忙碌（可重試）  */
    APP_EINVAL    = -4,   /* 參數錯誤            */
    APP_ENODEV    = -5,   /* 裝置不存在/無回應   */
    APP_ECRC      = -6,   /* 資料校驗失敗        */
    APP_ENOSPACE  = -7,   /* 緩衝區/空間不足     */
};

/* ---- 常用巨集 ---- */
#define APP_ARRAY_COUNT(a)   (sizeof(a) / sizeof((a)[0]))
#define APP_MIN(a, b)        (((a) < (b)) ? (a) : (b))
#define APP_MAX(a, b)        (((a) > (b)) ? (a) : (b))
#define APP_CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))
#define APP_UNUSED(x)        ((void)(x))

/* ---- 臨界區（PRIMASK 保存/還原，支援巢狀） ----
 * 用法：
 *   uint32_t key = app_enter_critical();
 *   ... 極短的共享資料存取 ...
 *   app_exit_critical(key);
 */
#if defined(__GNUC__) && (defined(__arm__) || defined(__thumb__))
static inline uint32_t app_enter_critical(void)
{
    uint32_t primask;
    __asm volatile ("mrs %0, primask" : "=r" (primask));
    __asm volatile ("cpsid i" ::: "memory");
    return primask;
}
static inline void app_exit_critical(uint32_t primask)
{
    __asm volatile ("msr primask, %0" :: "r" (primask) : "memory");
}
#else
/* 主機端單元測試（x86）環境：無中斷，臨界區為空操作 */
static inline uint32_t app_enter_critical(void) { return 0u; }
static inline void app_exit_critical(uint32_t primask) { (void)primask; }
#endif

#ifdef __cplusplus
}
#endif

#endif /* APP_COMMON_H */
