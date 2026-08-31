/**
 * @file    drv_gfx.h
 * @brief   輕量繪圖層（作用於 SSD1306 framebuffer）。
 *
 * 只實作本案 UI 需要的原語；大字採「5x7 整數倍放大」，
 * 用 475B 的單一字型表換到多種字級 —— 小 flash 的實用取捨。
 */
#ifndef DRV_GFX_H
#define DRV_GFX_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 設定安全區內縮（單位：像素）。
 *
 * 便宜 OLED 模組的外框常會遮住最邊緣 1~2 px；設定內縮後，
 * 所有繪圖座標平移 (left, top)、並裁切在內縮後的視窗內 ——
 * 版面程式碼照舊以 (0,0) 起算，實體上自動避開被遮的邊緣。
 * 預設值來自 app_config.h（APP_OLED_INSET_*），CLI `oled inset` 可現場調。
 */
void gfx_set_inset(uint8_t left, uint8_t top, uint8_t right, uint8_t bottom);

/** @return 內縮後的可繪寬/高（版面計算請用這兩個，勿寫死 128/64） */
uint8_t gfx_width(void);
uint8_t gfx_height(void);

void gfx_clear(void);
void gfx_pixel(int16_t x, int16_t y, bool on);
void gfx_hline(int16_t x, int16_t y, int16_t w, bool on);
void gfx_vline(int16_t x, int16_t y, int16_t h, bool on);
void gfx_rect(int16_t x, int16_t y, int16_t w, int16_t h, bool fill, bool on);

/** @brief 畫字串。scale=1 → 6x8 字格；scale=2 → 12x16 */
void gfx_text(int16_t x, int16_t y, uint8_t scale, const char *str);

/** @brief 水平置中畫字串 */
void gfx_text_center(int16_t y, uint8_t scale, const char *str);

/** @brief printf 風格（內部緩衝 64 字元；禁用 %f，浮點請用 fix100） */
void gfx_printf(int16_t x, int16_t y, uint8_t scale, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/** @brief 進度條（外框 + 填充），val 範圍 0..100 */
void gfx_progress(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t val);

#ifdef __cplusplus
}
#endif

#endif /* DRV_GFX_H */
