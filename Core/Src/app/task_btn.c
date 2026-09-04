/**
 * @file    task_btn.c
 * @brief   按鍵任務：8 鍵掃描 + 事件 → 業務動作 / BLE 上報。
 *
 * 鍵位配置（KEY0~KEY7 全部事件皆經 PROTO_T_BTN 上報、PC 端皆可映射）：
 *   KEY0（PB0）單擊/雙擊/長按  點擊計數 / 空中滑鼠切換 / 計數歸零（本地功能）
 *   KEY1（PA1）單擊/雙擊/長按  OLED 換頁 / 遙測開關 / 陀螺儀校正（本地功能）
 *   KEY2~KEY5(PA4~7) KEY6(PA0) KEY7(PB1)  手柄鍵：無本地功能，純上報
 *
 * 所有按鍵的事件（press/release/click/double/long）一律上報 ——
 * PC 端要拿有本地功能的 KEY1/KEY6 做進階應用也拿得到。
 * 手柄鍵停用雙擊偵測：press/release 本就即時，停用後 click 也零延遲。
 */
#include "app/app_tasks.h"
#include "app/app_config.h"
#include "bsp/bsp_board.h"
#include "bsp/bsp_uart.h"
#include "bsp/bsp_wdg.h"
#include "mw/button.h"
#include "mw/storage.h"
#include "drivers/imu.h"

#define KEY_COUNT 8u

static btn_t s_keys[KEY_COUNT];

static bool key_read(void *user)
{
    switch ((uintptr_t)user) {
    case 0u: return BSP_KEY0_PRESSED();   /* PB0（手柄鍵） */
    case 1u: return BSP_KEY1_PRESSED();   /* PA1（UI）     */
    case 2u: return BSP_KEY2_PRESSED();
    case 3u: return BSP_KEY3_PRESSED();
    case 4u: return BSP_KEY4_PRESSED();
    case 5u: return BSP_KEY5_PRESSED();
    case 6u: return BSP_KEY6_PRESSED();   /* PA0（計數等本地功能） */
    case 7u: return BSP_KEY7_PRESSED();   /* PB1（手柄鍵） */
    default: return false;
    }
}

/* KEY0：計數 / 空中滑鼠 / 歸零（本地功能） */
static void on_key0(btn_event_t evt)
{
    stor_record_t *rec = stor_get();

    switch (evt) {
    case BTN_EVT_DOUBLE: {
        /* 空中滑鼠模式切換：狀態在裝置端（OLED 可見、STAT 旗標同步），
         * 實際的姿態→游標換算在 PC 端執行 */
        app_state_t *app = app_state();
        app->air_mouse = !app->air_mouse;
        ui_notify(app->air_mouse ? "AIR MOUSE ON" : "AIR MOUSE OFF");
        comm_send_event(PROTO_EV_AIRMOUSE, app->air_mouse ? 1u : 0u);
        break;
    }

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

    /* 全鍵事件上報（btn_event_t 數值即協定 action 碼） */
    comm_send_btn(id, (uint8_t)evt);

    if (id == 0u) {
        on_key0(evt);
    } else if (id == 1u) {
        on_key1(evt);
    }
    /* KEY2~5/KEY6/KEY7：純上報，本地無動作 */
}

void task_btn_init(void)
{
    static const btn_config_t cfg = {
        .debounce_ms   = APP_BTN_DEBOUNCE_MS,
        .long_ms       = APP_BTN_LONG_MS,
        .double_gap_ms = APP_BTN_DOUBLE_GAP_MS,
    };
    for (uint8_t i = 0u; i < KEY_COUNT; i++) {
        btn_init(&s_keys[i], i, &cfg, key_read, (void *)(uintptr_t)i,
                 btn_dispatch, NULL);
        if ((i != 1u) && (i != 0u)) {
            /* 僅 KEY1（雙擊=遙測）與 KEY0（雙擊=空中滑鼠）需要雙擊；
             * 其餘手柄鍵停用雙擊換取零延遲單擊。代價是那兩鍵的單擊
             * 需等雙擊窗口確認（+250ms），換頁與計數皆非延遲敏感，可接受 */
            btn_enable_double(&s_keys[i], false);
        }
    }
}

void task_btn(void)
{
    uint32_t now = HAL_GetTick();
    for (uint8_t i = 0u; i < KEY_COUNT; i++) {
        btn_poll(&s_keys[i], now);
    }
    bsp_wdg_checkin(WDG_TASK_BTN);
}
