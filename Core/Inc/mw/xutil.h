/**
 * @file    xutil.h
 * @brief   小型格式化工具。
 *
 * 背景：newlib-nano 的 printf 預設不支援 %f（要加連結旗標且體積大增）。
 * 本專案角度/溫度以「×100 定點整數」流通，顯示時用 fix100_to_str
 * 轉字串，完全避開浮點 printf —— 小 flash MCU 的常見手法。
 */
#ifndef MW_XUTIL_H
#define MW_XUTIL_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 將 ×100 定點數轉為 "-12.34" 形式字串。
 * @return 寫入的字元數（不含 NUL）
 */
int fix100_to_str(char *buf, size_t cap, int32_t v);

/** @brief 秒數 → "HH:MM:SS"（cap 至少 9） */
int uptime_to_str(char *buf, size_t cap, uint32_t seconds);

#ifdef __cplusplus
}
#endif

#endif /* MW_XUTIL_H */
