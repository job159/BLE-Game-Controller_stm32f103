/**
 * @file    button.h
 * @brief   通用按鍵事件狀態機：去彈跳 / 單擊 / 雙擊 / 長按。
 *
 * 可移植性：模組不碰 GPIO —— 讀腳位透過函式指標注入，
 * 時間由呼叫者傳入。同一份程式碼可跑在任何 MCU 或 PC 單元測試。
 *
 * 事件語意：
 *   PRESS       實體按下（去彈跳後）立即發出
 *   RELEASE     實體放開立即發出
 *   CLICK       短按確認。若啟用雙擊偵測，會等 double_gap_ms
 *               確認沒有第二擊才發出（延遲 = 雙擊偵測的必要代價）
 *   DOUBLE      兩次短按間隔 < double_gap_ms
 *   LONG_PRESS  按住達 long_ms 時發出（此次放開不再發 CLICK）
 *
 * 若該鍵用不到雙擊（如本案 KEY0 計數鍵），呼叫 btn_enable_double(false)
 * 讓 CLICK 在放開瞬間發出，零延遲 —— 延遲與功能的取捨要顯式可選。
 */
#ifndef MW_BUTTON_H
#define MW_BUTTON_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_EVT_PRESS = 0,
    BTN_EVT_RELEASE,
    BTN_EVT_CLICK,
    BTN_EVT_DOUBLE,
    BTN_EVT_LONG_PRESS,
} btn_event_t;

typedef bool (*btn_read_fn_t)(void *user);              /* true = 實體按下 */
typedef void (*btn_event_cb_t)(uint8_t id, btn_event_t evt, void *user);

typedef struct {
    uint16_t debounce_ms;
    uint16_t long_ms;
    uint16_t double_gap_ms;
} btn_config_t;

typedef struct {
    /* 注入 */
    uint8_t         id;
    btn_read_fn_t   read;
    void           *read_user;
    btn_event_cb_t  cb;
    void           *cb_user;
    btn_config_t    cfg;
    bool            double_en;
    /* 內部狀態（呼叫者不可直接存取） */
    uint8_t  state;
    uint32_t t_edge;       /* 邊緣去彈跳計時起點 */
    uint32_t t_press;      /* PRESS 確認時刻（長按計時） */
    uint32_t t_click;      /* 第一次 CLICK 掛起時刻（雙擊窗口） */
    bool     raw_last;
    bool     long_fired;
    bool     is_double_press;
    bool     click_pending;
} btn_t;

void btn_init(btn_t *b, uint8_t id, const btn_config_t *cfg,
              btn_read_fn_t read, void *read_user,
              btn_event_cb_t cb, void *cb_user);

void btn_enable_double(btn_t *b, bool enable);

/** @brief 週期輪詢（建議 5~20ms 一次），now_ms 為毫秒時基 */
void btn_poll(btn_t *b, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* MW_BUTTON_H */
