/**
 * @file    task_ui.c
 * @brief   顯示任務：OLED 頁面渲染、提示條、狀態 LED。
 *
 * 頁面（KEY1 單擊循環）：
 *   0 SPLASH   開機畫面（1.5 秒後自動進入 DASH）
 *   1 DASH     姿態儀表板（roll/pitch/yaw 大字 + 點擊數 + 遙測狀態）
 *   2 STOR     EEPROM 記錄檢視（含未落盤 '*' 指示 —— 延遲寫入看得見）
 *   3 RAW      感測原始值（debug 視角）
 *   4 SYS      系統資訊（版本/運行時間/CPU/錯誤統計）
 *
 * LED 語言（狀態一眼可讀，量產除錯的老朋友）：
 *   短亮 100ms/秒  = 正常心跳
 *   5Hz 快閃       = 降級運行（IMU 或 EEPROM 離線）
 */
#include "app/app_tasks.h"
#include "app/app_config.h"
#include "bsp/bsp.h"
#include "bsp/bsp_board.h"
#include "bsp/bsp_uart.h"
#include "mw/sched.h"
#include "mw/storage.h"
#include "mw/xutil.h"
#include "drivers/drv_ssd1306.h"
#include "drivers/drv_gfx.h"
#include "drivers/drv_mpu6050.h"
#include "drivers/drv_at24cxx.h"
#include "drivers/imu.h"
#include <string.h>

#define PAGE_SPLASH  0u
#define PAGE_DASH    1u
#define PAGE_STOR    2u
#define PAGE_RAW     3u
#define PAGE_SYS     4u
#define PAGE_COUNT   5u      /* 換頁循環只在 1..4 之間 */

#define SPLASH_MS    1500u
#define TOAST_MS     1500u

static char     s_toast[22];
static uint32_t s_toast_tick;
static uint8_t  s_led_phase;

void task_ui_init(void)
{
    app_state()->ui_page = PAGE_SPLASH;
    gfx_set_inset(APP_OLED_INSET_LEFT, APP_OLED_INSET_TOP,
                  APP_OLED_INSET_RIGHT, APP_OLED_INSET_BOTTOM);
}

void ui_next_page(void)
{
    app_state_t *app = app_state();
    if (app->ui_page == PAGE_SPLASH) {
        return;
    }
    app->ui_page++;
    if (app->ui_page >= PAGE_COUNT) {
        app->ui_page = PAGE_DASH;
    }
}

void ui_notify(const char *msg)
{
    strncpy(s_toast, msg, sizeof(s_toast) - 1u);
    s_toast[sizeof(s_toast) - 1u] = '\0';
    s_toast_tick = HAL_GetTick();
}

/* ------------------------------------------------------------------ */
/*                            頁面渲染                                 */
/* ------------------------------------------------------------------ */

static void draw_splash(void)
{
    gfx_text_center(8, 2, "IMU-BLE");
    gfx_text_center(30, 1, "STM32F103 Node");
    gfx_text_center(44, 1, "v" APP_FW_VERSION);
}

static void draw_angle_row(int16_t y, char axis, float deg)
{
    char num[12];
    (void)fix100_to_str(num, sizeof(num), (int32_t)(deg * 100.0f));
    char line[16];
    line[0] = axis;
    line[1] = '\0';
    gfx_text(0, y, 2, line);
    gfx_printf(20, y, 2, "%8s", num);
}

static void draw_dash(void)
{
    const imu_state_t *imu = imu_get();
    uint8_t hz = comm_telemetry_hz();

    gfx_printf(0, 0, 1, "CLK %-6lu", (unsigned long)stor_get()->click_count);
    /* 右緣對齊安全區："TX 99Hz" = 7 字 × 6px = 42px */
    int16_t tx_x = (int16_t)(gfx_width() - 42u);
    if (hz > 0u) {
        gfx_printf(tx_x, 0, 1, "TX %2uHz", (unsigned)hz);
    } else {
        gfx_text(tx_x, 0, 1, "TX off");
    }
    gfx_hline(0, 9, (int16_t)gfx_width(), true);

    if (imu_healthy()) {
        /* 三列大字依安全區高度均分；scale-2 字模實高 14px（7 列×2），
         * 列距最低 15 仍留 1px 行距，內縮加深時自動壓縮而不被裁掉 */
        int16_t top = 11;
        int16_t pitch = (int16_t)((gfx_height() - top) / 3);
        if (pitch < 15) {
            pitch = 15;
        }
        draw_angle_row(top, 'R', imu->roll_deg);
        draw_angle_row((int16_t)(top + pitch), 'P', imu->pitch_deg);
        draw_angle_row((int16_t)(top + 2 * pitch), 'Y', imu->yaw_deg);
    } else {
        gfx_text_center(28, 2, "IMU FAIL");
    }
}

static void draw_stor(void)
{
    const stor_record_t *rec = stor_get();

    gfx_text(0, 0, 1, "EEPROM");
    if (stor_healthy()) {
        /* 顯示實際掃描到的裝置位址（模組焊法各異：0x50~0x57） */
        gfx_printf((int16_t)(gfx_width() - 60), 0, 1, "0x%02x %3uB",
                   at24_dev_addr(), (unsigned)at24_size());
    } else {
        gfx_printf((int16_t)(gfx_width() - 48), 0, 1, "OFFLINE");
    }
    gfx_hline(0, 9, (int16_t)gfx_width(), true);

    /* '*' = RAM 快取已變更、尚未寫入 EEPROM（延遲落盤進行中） */
    gfx_printf(0, 12, 1, "CLK %lu%s  BOOT %lu",
               (unsigned long)rec->click_count,
               stor_is_dirty() ? "*" : "",
               (unsigned long)rec->boot_count);
    gfx_printf(0, 22, 1, "HZ %u  FLG %02x  SEQ %u",
               (unsigned)rec->telemetry_hz, (unsigned)rec->flags,
               (unsigned)stor_seq());
    gfx_printf(0, 32, 1, "BIAS %d %d %d",
               rec->gyro_bias[0], rec->gyro_bias[1], rec->gyro_bias[2]);
    gfx_printf(0, (int16_t)(gfx_height() - 8u), 1, "SLOT %c  W %lu  F %lu",
               (stor_active_slot() == 0u) ? 'A' : 'B',
               (unsigned long)stor_stats()->commits,
               (unsigned long)stor_stats()->commit_fails);
}

static void draw_raw(void)
{
    mpu6050_raw_t raw;
    gfx_text(0, 0, 1, "RAW SENSOR");
    gfx_hline(0, 9, (int16_t)gfx_width(), true);

    if (imu_healthy() && (mpu6050_read_raw(&raw) == APP_OK)) {
        gfx_printf(0, 12, 1, "A %6d %6d %6d", raw.ax, raw.ay, raw.az);
        gfx_printf(0, 22, 1, "G %6d %6d %6d", raw.gx, raw.gy, raw.gz);
        char t[12];
        (void)fix100_to_str(t, sizeof(t), mpu6050_temp_cdegc(raw.temp_raw));
        gfx_printf(0, 32, 1, "T %sC", t);
    } else {
        gfx_text(0, 20, 1, "IMU offline");
    }
    gfx_printf(0, 42, 1, "INT %lu", (unsigned long)imu_int_count());
    gfx_printf(0, (int16_t)(gfx_height() - 8u), 1, "SMP %lu",
               (unsigned long)imu_get()->sample_count);
}

static void draw_sys(void)
{
    char up[10];
    (void)uptime_to_str(up, sizeof(up), HAL_GetTick() / 1000u);

    gfx_text(0, 0, 1, "SYSTEM");
    gfx_printf(60, 0, 1, "v" APP_FW_VERSION);
    gfx_hline(0, 9, (int16_t)gfx_width(), true);
    gfx_printf(0, 12, 1, "UP  %s  CPU %u%%", up, (unsigned)sched_cpu_percent());
    gfx_printf(0, 22, 1, "BOOT %lu  RST %s",
               (unsigned long)stor_get()->boot_count, bsp_reset_cause_str());
    gfx_printf(0, 32, 1, "BLE rx%lu tx%lu",
               (unsigned long)comm_proto_stats()->rx_frames,
               (unsigned long)app_state()->comm_tx_frames);
    gfx_printf(0, 42, 1, "CRC e%lu  EEP w%lu",
               (unsigned long)comm_proto_stats()->crc_errors,
               (unsigned long)stor_stats()->commits);
    gfx_printf(0, (int16_t)(gfx_height() - 8u), 1, "%s %s %s",
               imu_healthy() ? "IMU+" : "IMU-",
               stor_healthy() ? "EEP+" : "EEP-",
               oled_ok() ? "OLED+" : "OLED-");
}

/* 面板診斷測試圖（CLI `oled test` 切換）：
 * 外框可看出偏移/裁切（SH1106 未設偏移時右緣缺 2 欄）、
 * 左右兩排頁序號 0..7 可看出交錯/亂序（COM pins 錯誤時上下跳頁）、
 * 十字線可看出置中與鏡像。 */
static void draw_testpat(void)
{
    int16_t w = (int16_t)gfx_width();
    int16_t h = (int16_t)gfx_height();

    /* 外框貼齊「安全區」邊緣：四邊線都完整可見 = 內縮值正確 */
    gfx_rect(0, 0, w, h, false, true);
    gfx_vline((int16_t)(w / 2), 0, h, true);
    gfx_hline(0, (int16_t)(h / 2), w, true);
    for (uint8_t p = 0u; p < 8u; p++) {
        gfx_printf(3, (int16_t)(p * 8u), 1, "%u", (unsigned)p);
        gfx_printf((int16_t)(w - 9), (int16_t)(p * 8u), 1, "%u", (unsigned)p);
    }
    gfx_text_center(12, 1, "OLED TEST");
    gfx_text_center(44, 1, "0123456789");
}

static void draw_toast(void)
{
    if (s_toast[0] == '\0') {
        return;
    }
    if ((uint32_t)(HAL_GetTick() - s_toast_tick) >= TOAST_MS) {
        s_toast[0] = '\0';
        return;
    }
    /* 底部提示條（錨定安全區底緣）：清底 + 邊框 + 置中文字 */
    int16_t w = (int16_t)gfx_width();
    int16_t y0 = (int16_t)(gfx_height() - 12u);
    gfx_rect(0, y0, w, 12, true, false);
    gfx_rect(0, y0, w, 12, false, true);
    gfx_text_center((int16_t)(y0 + 2), 1, s_toast);
}

/* ------------------------------------------------------------------ */
/*                          LED 心跳樣式                               */
/* ------------------------------------------------------------------ */

static void led_update(void)
{
    bool degraded = !imu_healthy() || !stor_healthy();
    s_led_phase = (uint8_t)((s_led_phase + 1u) % 10u);   /* 100ms × 10 = 1s */

    if (degraded) {
        if ((s_led_phase % 2u) == 0u) {   /* 5Hz 快閃 */
            BSP_LED_TOGGLE();
        }
    } else {
        if (s_led_phase == 0u) {
            BSP_LED_ON();                 /* 每秒短亮 100ms */
        } else {
            BSP_LED_OFF();
        }
    }
}

/* ------------------------------------------------------------------ */
/*                              任務                                   */
/* ------------------------------------------------------------------ */

void task_ui(void)
{
    led_update();

    if (!oled_ok()) {
        return;   /* 無螢幕降級：LED 與 CLI 仍在服務 */
    }

    app_state_t *app = app_state();
    if ((app->ui_page == PAGE_SPLASH) && (HAL_GetTick() >= SPLASH_MS)) {
        app->ui_page = PAGE_DASH;
    }

    /* 前一幀 DMA 未送完就跳過本幀 —— 絕不阻塞等待 */
    if (oled_flush_busy()) {
        return;
    }

    gfx_clear();
    if (app->ui_test_mode) {
        draw_testpat();
    } else {
        switch (app->ui_page) {
        case PAGE_SPLASH: draw_splash(); break;
        case PAGE_DASH:   draw_dash();   break;
        case PAGE_STOR:   draw_stor();   break;
        case PAGE_RAW:    draw_raw();    break;
        case PAGE_SYS:    draw_sys();    break;
        default:          app->ui_page = PAGE_DASH; break;
        }
        draw_toast();
    }
    (void)oled_flush();
}
