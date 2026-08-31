/**
 * @file    drv_ssd1306.h
 * @brief   128x64 I2C OLED 驅動（SSD1306 / SSD1315 / SH1106 相容）。
 *
 * 市售「SSD1306 模組」實際晶片相當混亂（SSD1306 / SSD1315 / SH1106），
 * 差異點：
 *   - SH1106 只支援「分頁定址」，忽略 0x20 定址模式與 0x21/0x22 窗口
 *     命令 —— 用 SSD1306 水平串流去驅動它，畫面會滾動錯位（經典災情）。
 *   - SH1106 RAM 為 132 欄，可視區起點在第 2 欄（需欄位偏移）。
 *   - 升壓命令不同（SSD1306: 0x8D / SH1106: 0xAD）。
 *
 * 因此本驅動一律採「分頁定址」刷新（兩家都支援的交集）：
 *
 *   oled_flush() 啟動 → 每頁：3 byte 頁命令(阻塞 ~90µs) + 128B DMA
 *   → 下一頁由 oled_poll()（主迴圈每圈呼叫）在 DMA 完成後接力
 *   → 8 頁 ≈ 28ms 背景完成，主迴圈永不阻塞等待。
 *
 * 面板參數（控制器/COM 接線/旋轉）可於執行期切換（CLI `oled` 命令），
 * 現場找出正確組合後再固化到 app_config.h。
 */
#ifndef DRV_SSD1306_H
#define DRV_SSD1306_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_WIDTH   128u
#define OLED_HEIGHT  64u
#define OLED_PAGES   (OLED_HEIGHT / 8u)
#define OLED_FB_SIZE (OLED_WIDTH * OLED_HEIGHT / 8u)

/** 面板組態（執行期可調，預設值來自 app_config.h） */
typedef struct {
    bool    sh1106;      /* true = SH1106 命令集 + 2 欄偏移 */
    uint8_t com_pins;    /* 0xDA 參數：0x12=交錯(128x64 常見) / 0x02=順序 */
    bool    rotate180;   /* false = 0xA1+0xC8（模組排針朝上的常見方向） */
} oled_cfg_t;

/** @brief 初始化（自動探測 0x3C/0x3D）。失敗回 APP_ENODEV，系統可降級運行 */
int oled_init(void);

/** @brief 以目前組態重新初始化面板（CLI 診斷切換用） */
int oled_reinit(void);

bool oled_ok(void);
uint8_t oled_addr(void);
oled_cfg_t *oled_cfg(void);      /* 修改欄位後呼叫 oled_reinit() 生效 */

/** @return framebuffer 指標（gfx 層直接操作） */
uint8_t *oled_fb(void);

/**
 * @brief 啟動一次非同步全幀刷新（分頁接力由 oled_poll 推進）。
 * @return APP_OK 已啟動；APP_EBUSY 前一幀尚未送完（呼叫者跳過本幀即可）
 */
int oled_flush(void);

/** @brief 刷新狀態機泵：由主迴圈每圈呼叫（無刷新進行時零成本） */
void oled_poll(void);

/** @brief 阻塞式刷新（初始化/診斷用），逾時 100ms */
int oled_flush_sync(void);

bool oled_flush_busy(void);

void oled_set_contrast(uint8_t level);
void oled_display_on(bool on);

#ifdef __cplusplus
}
#endif

#endif /* DRV_SSD1306_H */
