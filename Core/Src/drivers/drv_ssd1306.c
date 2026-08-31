/**
 * @file    drv_ssd1306.c
 * @brief   128x64 OLED 驅動實作（SSD1306 / SSD1315 / SH1106 通用）。
 *
 * I2C 資料格式：控制位元組 0x00=命令流、0x40=資料流。
 * 刷新採分頁定址（page addressing，兩家控制器的共同交集；
 * 亦是 u8g2 等成熟驅動的作法），每頁 128B 走 DMA，
 * 頁與頁之間由 oled_poll() 在主迴圈接力 —— 詳見標頭說明。
 */
#include "drivers/drv_ssd1306.h"
#include "app/app_config.h"
#include "bsp/bsp_i2c.h"
#include "bsp/bsp_board.h"
#include <string.h>

#define CTRL_CMD      0x00u
#define CTRL_DATA     0x40u
#define CMD_TIMEOUT   20u
#define FLUSH_IDLE    0xFFu

static uint8_t s_fb[OLED_FB_SIZE];
static uint8_t s_addr;                    /* 實際探測到的位址 */
static bool    s_ok;
static volatile uint8_t s_flush_page = FLUSH_IDLE;   /* 下一個待送的頁 */

static oled_cfg_t s_cfg = {
    .sh1106    = (APP_OLED_SH1106 != 0),
    .com_pins  = APP_OLED_COM_PINS,
    .rotate180 = false,
};

static int cmds(const uint8_t *seq, uint16_t n)
{
    return bsp_i2c_mem_write(BSP_I2C_DISP, s_addr, CTRL_CMD, 1u,
                             seq, n, CMD_TIMEOUT);
}

/* 依目前組態送出初始化序列（不含 0x20 定址模式：
 * 兩家控制器重置預設皆為分頁定址，刷新路徑也只用分頁定址） */
static int send_init_seq(void)
{
    uint8_t seq[26];
    uint8_t n = 0u;

    seq[n++] = 0xAE;                                  /* display off */
    seq[n++] = 0xD5; seq[n++] = 0x80;                 /* clock divide */
    seq[n++] = 0xA8; seq[n++] = 0x3F;                 /* multiplex = 64 */
    seq[n++] = 0xD3; seq[n++] = 0x00;                 /* display offset */
    seq[n++] = 0x40;                                  /* start line = 0 */
    if (s_cfg.sh1106) {
        seq[n++] = 0xAD; seq[n++] = 0x8B;             /* SH1106 DC-DC on */
    } else {
        seq[n++] = 0x8D; seq[n++] = 0x14;             /* SSD1306 charge pump */
    }
    seq[n++] = s_cfg.rotate180 ? 0xA0 : 0xA1;         /* segment remap */
    seq[n++] = s_cfg.rotate180 ? 0xC0 : 0xC8;         /* COM scan dir */
    seq[n++] = 0xDA; seq[n++] = s_cfg.com_pins;       /* COM pins 接線 */
    seq[n++] = 0x81; seq[n++] = 0xCF;                 /* contrast */
    seq[n++] = 0xD9; seq[n++] = 0xF1;                 /* pre-charge */
    seq[n++] = 0xDB; seq[n++] = 0x30;                 /* VCOMH */
    seq[n++] = 0xA4;                                  /* resume from RAM */
    seq[n++] = 0xA6;                                  /* normal video */
    seq[n++] = 0xAF;                                  /* display on */

    return cmds(seq, n);
}

/* 設定頁位址與欄位址（SH1106 可視區自 RAM 第 2 欄起）並啟動該頁 DMA */
static int start_page(uint8_t page)
{
    uint8_t col = s_cfg.sh1106 ? 2u : 0u;
    uint8_t seq[3] = {
        (uint8_t)(0xB0u | page),
        (uint8_t)(0x00u | (col & 0x0Fu)),
        (uint8_t)(0x10u | (col >> 4)),
    };
    int rc = cmds(seq, sizeof(seq));
    if (rc != APP_OK) {
        return rc;
    }
    return bsp_i2c_write_stream_dma(BSP_I2C_DISP, s_addr, CTRL_DATA,
                                    &s_fb[(uint16_t)page * OLED_WIDTH],
                                    OLED_WIDTH);
}

int oled_reinit(void)
{
    if (s_addr == 0u) {
        return APP_ENODEV;
    }
    s_flush_page = FLUSH_IDLE;            /* 放棄進行中的刷新 */
    uint32_t t0 = HAL_GetTick();          /* 等在途 DMA 頁送完再下命令 */
    while (bsp_i2c_dma_busy(BSP_I2C_DISP)) {
        if ((uint32_t)(HAL_GetTick() - t0) > 50u) {
            break;
        }
    }
    int rc = send_init_seq();
    if (rc != APP_OK) {
        s_ok = false;
        return rc;
    }
    s_ok = true;
    return oled_flush_sync();
}

int oled_init(void)
{
    s_ok = false;
    s_addr = 0u;

    /* 模組位址可能為 0x3C 或 0x3D（D/C# 腳位落焊差異），自動探測 */
    if (bsp_i2c_probe(BSP_I2C_DISP, BSP_ADDR_SSD1306, CMD_TIMEOUT) == APP_OK) {
        s_addr = BSP_ADDR_SSD1306;
    } else if (bsp_i2c_probe(BSP_I2C_DISP, BSP_ADDR_SSD1306 + 1u,
                             CMD_TIMEOUT) == APP_OK) {
        s_addr = BSP_ADDR_SSD1306 + 1u;
    } else {
        return APP_ENODEV;
    }

    memset(s_fb, 0, sizeof(s_fb));
    return oled_reinit();
}

bool oled_ok(void)
{
    return s_ok;
}

uint8_t oled_addr(void)
{
    return s_addr;
}

oled_cfg_t *oled_cfg(void)
{
    return &s_cfg;
}

uint8_t *oled_fb(void)
{
    return s_fb;
}

int oled_flush(void)
{
    if (!s_ok) {
        return APP_ENODEV;
    }
    if (s_flush_page != FLUSH_IDLE) {
        return APP_EBUSY;
    }
    /* 立即送第 0 頁，其餘由 oled_poll() 接力 */
    int rc = start_page(0u);
    if (rc != APP_OK) {
        return rc;
    }
    s_flush_page = 1u;
    return APP_OK;
}

void oled_poll(void)
{
    uint8_t page = s_flush_page;
    if (page == FLUSH_IDLE) {
        return;                            /* 無刷新進行：零成本 */
    }
    if (bsp_i2c_dma_busy(BSP_I2C_DISP)) {
        return;                            /* 前一頁 DMA 還在跑 */
    }
    if (page >= OLED_PAGES) {
        s_flush_page = FLUSH_IDLE;         /* 8 頁送完，本幀結束 */
        return;
    }
    if (start_page(page) != APP_OK) {
        s_flush_page = FLUSH_IDLE;         /* 傳輸失敗：放棄本幀，
                                              錯誤已計入 i2c 統計 */
        return;
    }
    s_flush_page = (uint8_t)(page + 1u);
}

int oled_flush_sync(void)
{
    int rc = oled_flush();
    if (rc != APP_OK) {
        return rc;
    }
    uint32_t start = HAL_GetTick();
    while (oled_flush_busy()) {
        oled_poll();
        if ((uint32_t)(HAL_GetTick() - start) > 100u) {
            s_flush_page = FLUSH_IDLE;
            return APP_ETIMEOUT;
        }
    }
    return APP_OK;
}

bool oled_flush_busy(void)
{
    return (s_flush_page != FLUSH_IDLE) || bsp_i2c_dma_busy(BSP_I2C_DISP);
}

void oled_set_contrast(uint8_t level)
{
    uint8_t seq[] = { 0x81, level };
    (void)cmds(seq, sizeof(seq));
}

void oled_display_on(bool on)
{
    uint8_t cmd = on ? 0xAFu : 0xAEu;
    (void)cmds(&cmd, 1u);
}
