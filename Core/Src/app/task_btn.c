/**
 * @file    task_btn.c
 * @brief   按鍵任務：掃描 + 事件 → 業務動作對映。
 *
 * 鍵位配置：
 *   KEY0 單擊     累計點擊次數（寫入 EEPROM，延遲落盤防磨耗）
 *   KEY0 長按     點擊次數歸零（立即落盤）
 *   KEY1 單擊     OLED 換頁
 *   KEY1 雙擊     遙測開/關切換
 *   KEY1 長按     觸發陀螺儀校正
 *
 * KEY0 停用雙擊偵測 → 單擊零延遲（計數手感優先）；
 * KEY1 保留雙擊 → 單擊有 250ms 確認延遲（換頁無感）。
 * 同一顆狀態機、不同組態 —— 延遲與功能的取捨顯式化。
 */
#include "app/app_tasks.h"
#include "app/app_config.h"
#include "bsp/bsp_board.h"
#include "bsp/bsp_uart.h"
#include "bsp/bsp_wdg.h"
#include "mw/button.h"
#include "mw/storage.h"
#include "drivers/imu.h"

#define BTN_ID_KEY0  0u
#define BTN_ID_KEY1  1u

static btn_t s_key0;
static btn_t s_key1;

static bool key0_read(void *user)
{
    APP_UNUSED(user);
    return BSP_KEY0_PRESSED();
}

static bool key1_read(void *user)
{
    APP_UNUSED(user);
    return BSP_KEY1_PRESSED();
}

static void on_key0(btn_event_t evt)
{
    stor_record_t *rec = stor_get();

    switch (evt) {
    case BTN_EVT_CLICK:
        rec->click_count++;
        stor_mark_dirty();               /* 靜止 2 秒後自動落盤 */
        dbg_printf("[btn ] click #%lu\r\n", (unsigned long)rec->click_count);
        comm_send_event(PROTO_EV_BTN_CLICK, rec->click_count);
        break;

    case BTN_EVT_LONG_PRESS:
        rec->click_count = 0u;
        stor_mark_dirty();
        (void)stor_commit_now();         /* 使用者主動歸零：立即落盤 */
        dbg_printf("[btn ] click counter reset\r\n");
        ui_notify("CLICKS RESET");
        comm_send_event(PROTO_EV_CLICKS_RESET, 0u);
        break;

    default:
        break;
    }
}

static void on_key1(btn_event_t evt)
{
    switch (evt) {
    case BTN_EVT_CLICK:
        ui_next_page();
        break;

    case BTN_EVT_DOUBLE: {
        /* 遙測開關：0 ↔ 上次頻率（無上次值則用預設） */
        uint8_t hz = comm_telemetry_hz();
        if (hz > 0u) {
            comm_set_telemetry_hz(0u);
            ui_notify("TELEMETRY OFF");
            comm_send_event(PROTO_EV_BTN_DOUBLE, 0u);
        } else {
            uint8_t restore = stor_get()->telemetry_hz;
            if ((restore == 0u) || (restore > APP_TELEMETRY_HZ_MAX)) {
                restore = APP_TELEMETRY_HZ_DEF;
            }
            comm_set_telemetry_hz(restore);
            ui_notify("TELEMETRY ON");
            comm_send_event(PROTO_EV_BTN_DOUBLE, restore);
        }
        break;
    }

    case BTN_EVT_LONG_PRESS:
        if (imu_healthy()) {
            app_state()->cal_request = true;   /* task_imu 非同步執行 */
        } else {
            ui_notify("IMU OFFLINE");
        }
        break;

    default:
        break;
    }
}

static void btn_dispatch(uint8_t id, btn_event_t evt, void *user)
{
    APP_UNUSED(user);
    if (id == BTN_ID_KEY0) {
        on_key0(evt);
    } else {
        on_key1(evt);
    }
}

void task_btn_init(void)
{
    static const btn_config_t cfg = {
        .debounce_ms   = APP_BTN_DEBOUNCE_MS,
        .long_ms       = APP_BTN_LONG_MS,
        .double_gap_ms = APP_BTN_DOUBLE_GAP_MS,
    };
    btn_init(&s_key0, BTN_ID_KEY0, &cfg, key0_read, NULL, btn_dispatch, NULL);
    btn_init(&s_key1, BTN_ID_KEY1, &cfg, key1_read, NULL, btn_dispatch, NULL);
    btn_enable_double(&s_key0, false);   /* 計數鍵：單擊零延遲 */
}

void task_btn(void)
{
    uint32_t now = HAL_GetTick();
    btn_poll(&s_key0, now);
    btn_poll(&s_key1, now);
    bsp_wdg_checkin(WDG_TASK_BTN);
}
