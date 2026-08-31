/**
 * @file    task_comm.c
 * @brief   BLE 通訊任務：收包解析 / 命令處理 / 遙測發送 / 橋接模式。
 *
 * 資料流：
 *   RX：ble_read() → proto_parser_feed() → on_frame() → 執行 + ACK
 *   TX：payload 結構 → proto_build() → ble_write()（DMA 背景送出）
 *
 * 橋接模式（bridge）：把 USART1 與 USART2 直通，PC 終端機可直接
 * 對 nRF52832 下 AT 命令（改名、改鮑率）—— 免拔線改參數，
 * 真實產品的產測/維修常備通道。Ctrl+]（0x1D）離開。
 */
#include "app/app_tasks.h"
#include "app/app_config.h"
#include "bsp/bsp.h"
#include "bsp/bsp_uart.h"
#include "bsp/bsp_wdg.h"
#include "mw/proto.h"
#include "mw/sched.h"
#include "mw/storage.h"
#include "mw/cli.h"
#include "drivers/imu.h"
#include "stm32f1xx_hal.h"
#include <string.h>

#define BRIDGE_EXIT_CHAR  0x1D   /* Ctrl+] */

static proto_parser_t s_parser;
static uint8_t s_tx_seq;

/* ------------------------------------------------------------------ */
/*                            發送輔助                                 */
/* ------------------------------------------------------------------ */

void comm_send(uint8_t type, const void *payload, uint8_t len)
{
    uint8_t buf[PROTO_MAX_PAYLOAD + PROTO_OVERHEAD];
    uint16_t n = proto_build(buf, sizeof(buf), type, s_tx_seq++, payload, len);
    if (n > 0u) {
        (void)ble_write(buf, n);
        app_state()->comm_tx_frames++;
    }
}

/* 檔內慣用短名 */
static void send_frame(uint8_t type, const void *payload, uint8_t len)
{
    comm_send(type, payload, len);
}

static void send_ack(const proto_frame_t *req, uint8_t status)
{
    proto_ack_t ack = {
        .req_type = req->type,
        .req_seq  = req->seq,
        .status   = status,
    };
    send_frame(PROTO_T_ACK, &ack, sizeof(ack));
}

void comm_send_event(uint8_t ev_id, uint32_t arg)
{
    proto_event_t ev = { .id = ev_id, .arg = arg };
    send_frame(PROTO_T_EVENT, &ev, sizeof(ev));
}

void comm_send_btn(uint8_t key_id, uint8_t action)
{
    if (app_state()->bridge_mode) {
        return;   /* 橋接期間通道非協定資料，不上報 */
    }
    proto_btn_t btn = { .key_id = key_id, .action = action };
    send_frame(PROTO_T_BTN, &btn, sizeof(btn));
}

/* 回送一鍵映射：payload = {key_id, len, spec[len]} */
static void send_keymap_entry(uint8_t key_id)
{
    uint8_t payload[2u + PROTO_KEYMAP_SPEC_MAX];
    char spec[STOR_KEYMAP_SPEC_MAX + 1u];
    int n = stor_keymap_get(key_id, spec, sizeof(spec));
    if (n < 0) {
        n = 0;
    }
    payload[0] = key_id;
    payload[1] = (uint8_t)n;
    memcpy(&payload[2], spec, (size_t)n);
    send_frame(PROTO_T_KEYMAP, payload, (uint8_t)(2 + n));
}

static void send_info(void)
{
    proto_info_t info;
    memset(&info, 0, sizeof(info));
    info.proto_ver = APP_PROTO_VERSION;
    strncpy(info.fw_ver, APP_FW_VERSION, sizeof(info.fw_ver) - 1u);
    /* STM32F1 96-bit 唯一 ID（每顆晶片出廠燒錄，裝置識別用） */
    const uint32_t *uid = (const uint32_t *)UID_BASE;
    info.uid[0] = uid[0];
    info.uid[1] = uid[1];
    info.uid[2] = uid[2];
    send_frame(PROTO_T_INFO, &info, sizeof(info));
}

/* ------------------------------------------------------------------ */
/*                            命令處理                                 */
/* ------------------------------------------------------------------ */

uint8_t comm_telemetry_hz(void)
{
    if (g_task_id_comm_tx < 0) {
        return 0u;
    }
    const sched_task_info_t *info = sched_task_info(g_task_id_comm_tx);
    if ((info == NULL) || (info->period_ms == 0u)) {
        return 0u;
    }
    return (uint8_t)(1000u / info->period_ms);
}

void comm_set_telemetry_hz(uint8_t hz)
{
    if (hz > APP_TELEMETRY_HZ_MAX) {
        hz = APP_TELEMETRY_HZ_MAX;
    }
    (void)sched_set_period(g_task_id_comm_tx, (hz > 0u) ? (1000u / hz) : 0u);
    if (hz > 0u) {
        /* 只持久化「有效頻率」：關閉是暫時狀態，重開機恢復回報 */
        stor_get()->telemetry_hz = hz;
        stor_mark_dirty();
    }
    dbg_printf("[comm] telemetry rate -> %u Hz\r\n", (unsigned)hz);
}

const proto_stats_t *comm_proto_stats(void)
{
    return &s_parser.stats;
}

static void on_frame(const proto_frame_t *f, void *user)
{
    APP_UNUSED(user);

    switch (f->type) {
    case PROTO_T_PING:
        send_ack(f, PROTO_ACK_OK);
        break;

    case PROTO_T_SET_RATE:
        if (f->len != 1u) {
            send_ack(f, PROTO_ACK_ERR);
        } else if (f->payload[0] > APP_TELEMETRY_HZ_MAX) {
            send_ack(f, PROTO_ACK_ERR);
        } else {
            comm_set_telemetry_hz(f->payload[0]);
            send_ack(f, PROTO_ACK_OK);
        }
        break;

    case PROTO_T_RESET_CLICKS:
        stor_get()->click_count = 0u;
        stor_mark_dirty();
        (void)stor_commit_now();
        send_ack(f, PROTO_ACK_OK);
        comm_send_event(PROTO_EV_CLICKS_RESET, 0u);
        ui_notify("CLICKS RESET");
        break;

    case PROTO_T_CAL_GYRO:
        if (!imu_healthy()) {
            send_ack(f, PROTO_ACK_ERR);
        } else if (app_state()->cal_request) {
            send_ack(f, PROTO_ACK_BUSY);
        } else {
            app_state()->cal_request = true;   /* 受理；結果經 EVENT 回報 */
            send_ack(f, PROTO_ACK_OK);
        }
        break;

    case PROTO_T_GET_INFO:
        send_ack(f, PROTO_ACK_OK);
        send_info();
        break;

    case PROTO_T_SET_KEYMAP:
        /* payload = {key_id, len, spec[len]}，len 欄位須與框架長度一致 */
        if ((f->len < 2u) || (f->payload[1] != (uint8_t)(f->len - 2u))) {
            send_ack(f, PROTO_ACK_ERR);
        } else if (stor_keymap_set(f->payload[0],
                                   (const char *)&f->payload[2],
                                   f->payload[1]) == APP_OK) {
            send_ack(f, PROTO_ACK_OK);   /* 已入 RAM，storage 任務稍後落盤 */
        } else {
            send_ack(f, PROTO_ACK_ERR);
        }
        break;

    case PROTO_T_GET_KEYMAP:
        send_ack(f, PROTO_ACK_OK);
        for (uint8_t k = STOR_KEYMAP_FIRST_KEY;
             k < STOR_KEYMAP_FIRST_KEY + STOR_KEYMAP_KEYS; k++) {
            send_keymap_entry(k);
        }
        break;

    default:
        send_ack(f, PROTO_ACK_UNKNOWN);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*                              任務                                   */
/* ------------------------------------------------------------------ */

void task_comm_init(void)
{
    proto_parser_init(&s_parser, on_frame, NULL);
    /* 開機事件：主機端看到 BOOT + 重置原因即可判斷裝置是否異常重啟 */
    comm_send_event(PROTO_EV_BOOT, bsp_reset_cause());
}

static void bridge_poll(void)
{
    uint8_t buf[64];

    /* PC → BLE 模組 */
    int c;
    while ((c = dbg_getc()) >= 0) {
        if (c == BRIDGE_EXIT_CHAR) {
            app_state()->bridge_mode = false;
            dbg_printf("\r\n[comm] bridge mode exit\r\n");
            return;
        }
        uint8_t byte = (uint8_t)c;
        (void)ble_write(&byte, 1u);
    }
    /* BLE 模組 → PC */
    uint16_t n = ble_read(buf, sizeof(buf));
    if (n > 0u) {
        (void)dbg_write(buf, n);
    }
}

void task_comm_rx(void)
{
    if (app_state()->bridge_mode) {
        bridge_poll();
    } else {
        uint8_t buf[64];
        uint16_t n;
        /* 一次任務最多消化 128B（兩輪），避免突發資料撐爆時間片 */
        for (int i = 0; i < 2; i++) {
            n = ble_read(buf, sizeof(buf));
            if (n == 0u) {
                break;
            }
            proto_parser_feed(&s_parser, buf, n);
        }
#if APP_USE_CLI
        /* CLI 與 bridge 互斥消費除錯埠 RX，因此掛在同一任務分流 */
        cli_poll();
#endif
    }
    bsp_wdg_checkin(WDG_TASK_COMM);
}

void task_comm_tx(void)
{
    if (app_state()->bridge_mode) {
        return;   /* 橋接期間保持通道乾淨 */
    }
    const imu_state_t *imu = imu_get();
    proto_attitude_t att = {
        .roll_cdeg   = (int16_t)APP_CLAMP(imu->roll_deg * 100.0f, -32000.0f, 32000.0f),
        .pitch_cdeg  = (int16_t)APP_CLAMP(imu->pitch_deg * 100.0f, -32000.0f, 32000.0f),
        .yaw_cdeg    = (int16_t)APP_CLAMP(imu->yaw_deg * 100.0f, -32000.0f, 32000.0f),
        .temp_cdegc  = imu->temp_cdegc,
        .click_count = stor_get()->click_count,
        .uptime_ms   = HAL_GetTick(),
    };
    send_frame(PROTO_T_ATTITUDE, &att, sizeof(att));
}
