/**
 * @file    task_btn.c
 * @brief   按鍵任務：6 鍵掃描 + 事件 → 業務動作 / BLE 上報。
 *
 * 鍵位配置：
 *   KEY0 單擊/長按  點擊計數 +1（EEPROM 持久化）/ 計數歸零
 *   KEY1 單擊/雙擊/長按  OLED 換頁 / 遙測開關 / 陀螺儀校正
 *   KEY2~KEY5       手柄鍵：無本地功能，事件即時上報 PC 端
 *                   由 host_gui.py 映射成鍵盤/滑鼠動作
 *
 * 所有按鍵的事件（press/release/click/double/long）一律經
 * PROTO_T_BTN 上報 —— PC 端要拿 KEY0/KEY1 做進階應用也拿得到。
 * 手柄鍵停用雙擊偵測：press/release 本就即時，停用後 click
 * 也零延遲，把整條鏈路的延遲留給 BLE 而不是韌體。
 */
#include "app/app_tasks.h"
#include "app/app_config.h"
#include "bsp/bsp_board.h"
#include "bsp/bsp_uart.h"
#include "bsp/bsp_wdg.h"
#include "mw/button.h"
#include "mw/storage.h"
#include "drivers/imu.h"

#define KEY_COUNT 6u

static btn_t s_keys[KEY_COUNT];

static bool key_read(void *user)
{
    switch ((uintptr_t)user) {
    case 0u: return BSP_KEY0_PRESSED();
    case 1u: return BSP_KEY1_PRESSED();
    case 2u: return BSP_KEY2_PRESSED();
    case 3u: return BSP_KEY3_PRESSED();
    case 4u: return BSP_KEY4_PRESSED();
    case 5u: return BSP_KEY5_PRESSED();
    default: return false;
    }
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

    /* 全鍵事件上報（btn_event_t 數值即協定 action 碼） */
    comm_send_btn(id, (uint8_t)evt);

    if (id == 0u) {
        on_key0(evt);
    } else if (id == 1u) {
        on_key1(evt);
    }
    /* KEY2~KEY5：純上報，本地無動作 */
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
        if (i != 1u) {
            /* 僅 KEY1 需要雙擊；其餘停用換取零延遲單擊 */
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
