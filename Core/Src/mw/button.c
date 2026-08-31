/**
 * @file    button.c
 * @brief   按鍵事件狀態機實作。
 *
 * 狀態轉移圖：
 *
 *   IDLE ──raw──▶ DEB_PRESS ──穩定──▶ PRESSED ──放開──▶ DEB_REL ──穩定──▶ IDLE
 *    ▲              │(彈跳)              │(按住 long_ms)         (發 RELEASE/
 *    └──────────────┘                    ▼                        CLICK 判定)
 *                                     LONG_PRESS 事件
 *
 * 雙擊：第一次 CLICK 先掛起（click_pending），double_gap_ms 內
 * 出現第二次按下 → 發 DOUBLE；逾時 → 補發 CLICK。
 */
#include "mw/button.h"
#include <string.h>

enum {
    S_IDLE = 0,
    S_DEB_PRESS,
    S_PRESSED,
    S_DEB_REL,
};

static void emit(btn_t *b, btn_event_t evt)
{
    if (b->cb != NULL) {
        b->cb(b->id, evt, b->cb_user);
    }
}

void btn_init(btn_t *b, uint8_t id, const btn_config_t *cfg,
              btn_read_fn_t read, void *read_user,
              btn_event_cb_t cb, void *cb_user)
{
    memset(b, 0, sizeof(*b));
    b->id = id;
    b->read = read;
    b->read_user = read_user;
    b->cb = cb;
    b->cb_user = cb_user;
    b->cfg = *cfg;
    b->double_en = true;
    b->state = S_IDLE;
}

void btn_enable_double(btn_t *b, bool enable)
{
    b->double_en = enable;
}

void btn_poll(btn_t *b, uint32_t now)
{
    if (b->read == NULL) {
        return;
    }
    bool raw = b->read(b->read_user);
    b->raw_last = raw;

    switch (b->state) {
    case S_IDLE:
        if (raw) {
            b->t_edge = now;
            b->state = S_DEB_PRESS;
        } else if (b->click_pending &&
                   (uint32_t)(now - b->t_click) >= b->cfg.double_gap_ms) {
            /* 雙擊窗口逾時 → 確認為單擊 */
            b->click_pending = false;
            emit(b, BTN_EVT_CLICK);
        }
        break;

    case S_DEB_PRESS:
        if (!raw) {
            b->state = S_IDLE;   /* 彈跳，放棄 */
        } else if ((uint32_t)(now - b->t_edge) >= b->cfg.debounce_ms) {
            emit(b, BTN_EVT_PRESS);
            b->t_press = now;
            b->long_fired = false;
            if (b->click_pending && b->double_en &&
                (uint32_t)(now - b->t_click) < b->cfg.double_gap_ms) {
                b->click_pending = false;
                b->is_double_press = true;
                emit(b, BTN_EVT_DOUBLE);
            } else {
                b->is_double_press = false;
            }
            b->state = S_PRESSED;
        }
        break;

    case S_PRESSED:
        if (!raw) {
            b->t_edge = now;
            b->state = S_DEB_REL;
        } else if (!b->long_fired && !b->is_double_press &&
                   (uint32_t)(now - b->t_press) >= b->cfg.long_ms) {
            b->long_fired = true;
            emit(b, BTN_EVT_LONG_PRESS);
        }
        break;

    case S_DEB_REL:
        if (raw) {
            b->state = S_PRESSED;   /* 彈跳，仍按著 */
        } else if ((uint32_t)(now - b->t_edge) >= b->cfg.debounce_ms) {
            emit(b, BTN_EVT_RELEASE);
            if (!b->long_fired && !b->is_double_press) {
                if (b->double_en) {
                    b->click_pending = true;   /* 掛起，等雙擊窗口 */
                    b->t_click = now;
                } else {
                    emit(b, BTN_EVT_CLICK);    /* 零延遲單擊 */
                }
            }
            b->state = S_IDLE;
        }
        break;

    default:
        b->state = S_IDLE;
        break;
    }
}
