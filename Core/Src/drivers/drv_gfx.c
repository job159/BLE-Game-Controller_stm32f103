/**
 * @file    drv_gfx.c
 * @brief   繪圖層實作。
 *
 * framebuffer 記憶體排列（SSD1306 page 結構）：
 *   fb[x + (y >> 3) * 128] 的 bit(y & 7) 對應像素 (x, y)。
 */
#include "drivers/drv_gfx.h"
#include "drivers/drv_ssd1306.h"
#include "drivers/drv_font.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* 安全區 viewport：平移 + 裁切（見標頭說明） */
static uint8_t s_off_x;
static uint8_t s_off_y;
static uint8_t s_w = (uint8_t)OLED_WIDTH;
static uint8_t s_h = (uint8_t)OLED_HEIGHT;

void gfx_set_inset(uint8_t left, uint8_t top, uint8_t right, uint8_t bottom)
{
    /* 防呆：內縮到不足半個螢幕視為錯誤參數，直接忽略 */
    if (((uint16_t)left + right >= OLED_WIDTH / 2u) ||
        ((uint16_t)top + bottom >= OLED_HEIGHT / 2u)) {
        return;
    }
    s_off_x = left;
    s_off_y = top;
    s_w = (uint8_t)(OLED_WIDTH - left - right);
    s_h = (uint8_t)(OLED_HEIGHT - top - bottom);
}

uint8_t gfx_width(void)
{
    return s_w;
}

uint8_t gfx_height(void)
{
    return s_h;
}

void gfx_clear(void)
{
    memset(oled_fb(), 0, OLED_FB_SIZE);
}

void gfx_pixel(int16_t x, int16_t y, bool on)
{
    if ((x < 0) || (x >= (int16_t)s_w) ||
        (y < 0) || (y >= (int16_t)s_h)) {
        return;                       /* 超出安全區：裁切 */
    }
    uint16_t px = (uint16_t)x + s_off_x;
    uint16_t py = (uint16_t)y + s_off_y;
    uint8_t *fb = oled_fb();
    uint16_t idx = px + (py >> 3) * OLED_WIDTH;
    uint8_t  bit = (uint8_t)(1u << (py & 7u));
    if (on) {
        fb[idx] |= bit;
    } else {
        fb[idx] &= (uint8_t)~bit;
    }
}

void gfx_hline(int16_t x, int16_t y, int16_t w, bool on)
{
    for (int16_t i = 0; i < w; i++) {
        gfx_pixel((int16_t)(x + i), y, on);
    }
}

void gfx_vline(int16_t x, int16_t y, int16_t h, bool on)
{
    for (int16_t i = 0; i < h; i++) {
        gfx_pixel(x, (int16_t)(y + i), on);
    }
}

void gfx_rect(int16_t x, int16_t y, int16_t w, int16_t h, bool fill, bool on)
{
    if ((w <= 0) || (h <= 0)) {
        return;
    }
    if (fill) {
        for (int16_t j = 0; j < h; j++) {
            gfx_hline(x, (int16_t)(y + j), w, on);
        }
    } else {
        gfx_hline(x, y, w, on);
        gfx_hline(x, (int16_t)(y + h - 1), w, on);
        gfx_vline(x, y, h, on);
        gfx_vline((int16_t)(x + w - 1), y, h, on);
    }
}

static void draw_char(int16_t x, int16_t y, uint8_t scale, char ch)
{
    if ((ch < (char)FONT_FIRST_CHAR) || (ch > (char)FONT_LAST_CHAR)) {
        ch = '?';
    }
    const uint8_t *glyph = g_font5x7[(uint8_t)ch - FONT_FIRST_CHAR];

    for (uint8_t col = 0; col < FONT_W; col++) {
        uint8_t bits = glyph[col];
        for (uint8_t row = 0; row < FONT_H; row++) {
            bool on = (bits >> row) & 1u;
            if (!on) {
                continue;   /* 背景由呼叫者先清（gfx_clear/rect） */
            }
            if (scale == 1u) {
                gfx_pixel((int16_t)(x + col), (int16_t)(y + row), true);
            } else {
                gfx_rect((int16_t)(x + col * scale),
                         (int16_t)(y + row * scale),
                         scale, scale, true, true);
            }
        }
    }
}

void gfx_text(int16_t x, int16_t y, uint8_t scale, const char *str)
{
    if ((str == NULL) || (scale == 0u)) {
        return;
    }
    int16_t cx = x;
    while (*str != '\0') {
        draw_char(cx, y, scale, *str++);
        cx = (int16_t)(cx + (int16_t)(FONT_CELL_W * scale));
    }
}

void gfx_text_center(int16_t y, uint8_t scale, const char *str)
{
    if (str == NULL) {
        return;
    }
    int16_t w = (int16_t)(strlen(str) * FONT_CELL_W * scale);
    int16_t x = (int16_t)(((int16_t)gfx_width() - w) / 2);
    gfx_text((x > 0) ? x : 0, y, scale, str);
}

void gfx_printf(int16_t x, int16_t y, uint8_t scale, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gfx_text(x, y, scale, buf);
}

void gfx_progress(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t val)
{
    if (val > 100u) {
        val = 100u;
    }
    gfx_rect(x, y, w, h, false, true);
    int16_t fill_w = (int16_t)(((int32_t)(w - 2) * val) / 100);
    if (fill_w > 0) {
        gfx_rect((int16_t)(x + 1), (int16_t)(y + 1), fill_w,
                 (int16_t)(h - 2), true, true);
    }
}
