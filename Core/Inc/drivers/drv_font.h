/**
 * @file    drv_font.h
 * @brief   5x7 點陣字型（ASCII 0x20~0x7E）。
 *
 * 每字 5 bytes，一 byte 一直行，bit0 = 最上方像素。
 * 實際排版佔 6x8（右側留 1 px 字距）；gfx 層可整數倍放大。
 */
#ifndef DRV_FONT_H
#define DRV_FONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_FIRST_CHAR  0x20u
#define FONT_LAST_CHAR   0x7Eu
#define FONT_W           5u
#define FONT_H           7u
#define FONT_CELL_W      6u   /* 含字距 */
#define FONT_CELL_H      8u

extern const uint8_t g_font5x7[95][5];

#ifdef __cplusplus
}
#endif

#endif /* DRV_FONT_H */
