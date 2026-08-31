/**
 * @file    drv_blemod.h
 * @brief   BLE 透傳模組自動佈建（HC-42 等 AT 命令模組）。
 *
 * 解決的問題：模組出廠鮑率（HC-42 為 9600）≠ 韌體的 115200，
 * 使用者又不一定有 USB-TTL 可以走橋接模式手動下 AT。
 *
 * 做法（裝置端自主完成，免 PC）：
 *   1. 依序以候選鮑率對模組發 "AT\r\n" 探測（HC 系列未被 BLE
 *      連線時將 UART 輸入視為 AT 命令，回 "OK"）。
 *   2. 在非目標鮑率找到模組 → 下 "AT+BAUD=<target>" 永久改寫。
 *   3. 把 USART2 切回目標鮑率並驗證。
 *
 * 限制（誠實聲明）：
 *   - 模組已被 BLE 連線時處於透傳狀態、不吃 AT → 探測全數失敗，
 *     回 NOT_FOUND；下次開機（未連線時）會再自動嘗試。
 *   - 僅支援 CRLF 結尾的 AT 方言（HC-42/HC-08 家族）；HM-10 式
 *     無結尾方言不在支援範圍（那類模組請走 CLI bridge 手動設定）。
 */
#ifndef DRV_BLEMOD_H
#define DRV_BLEMOD_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLEMOD_OK_ALREADY = 0,   /* 模組已在目標鮑率，未做任何變更 */
    BLEMOD_OK_FIXED,         /* 已改寫模組鮑率至目標值 */
    BLEMOD_NOT_FOUND,        /* 所有候選鮑率皆無 AT 回應
                                （無模組 / 已被 BLE 連線 / 非 AT 方言） */
} blemod_result_t;

/**
 * @brief 自動鮑率佈建（阻塞最多約 1.5 秒；內部有餵狗）。
 * @param target_baud     目標鮑率（韌體 USART2 的工作鮑率）
 * @param[out] found_baud 探測到模組時回填其原鮑率；可為 NULL
 * @note  無論結果為何，返回時 USART2 已切回 target_baud。
 */
blemod_result_t blemod_autobaud(uint32_t target_baud, uint32_t *found_baud);

#ifdef __cplusplus
}
#endif

#endif /* DRV_BLEMOD_H */
