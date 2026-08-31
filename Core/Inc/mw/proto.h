/**
 * @file    proto.h
 * @brief   IMU-BLE Node 二進位通訊協定（框架層 + 訊息定義）。
 *
 * 框架格式（完整規格見 Docs/protocol.md）：
 *
 *   ┌──────┬──────┬──────┬─────┬─────┬─────────┬──────────┐
 *   │ 0xAA │ 0x55 │ TYPE │ SEQ │ LEN │ PAYLOAD │ CRC16 LE │
 *   └──────┴──────┴──────┴─────┴─────┴─────────┴──────────┘
 *     SOF0   SOF1   1B     1B    1B    0..96B     2B
 *
 *   CRC16-CCITT-FALSE 涵蓋 TYPE..PAYLOAD（不含 SOF）。
 *   多位元組欄位一律 little-endian（與 Cortex-M / x86 原生序一致）。
 *
 * 解析器為位元組串流狀態機：
 *  - 與傳輸層完全解耦（餵位元組進來、吐完整封包出去），
 *    同一套程式碼可掛在 UART、BLE、甚至檔案回放上。
 *  - CRC 錯誤或逾長自動重新同步（丟到下一個 0xAA），並計數供診斷。
 */
#ifndef MW_PROTO_H
#define MW_PROTO_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_SOF0          0xAAu
#define PROTO_SOF1          0x55u
#define PROTO_MAX_PAYLOAD   96u
#define PROTO_OVERHEAD      7u    /* 2 SOF + 3 header + 2 CRC */

/* ---- 訊息型別：裝置 → 主機 ---- */
#define PROTO_T_ATTITUDE    0x01u  /* 姿態遙測（週期性） */
#define PROTO_T_SYSSTAT     0x02u  /* 系統狀態（1Hz）    */
#define PROTO_T_EVENT       0x03u  /* 非同步事件         */
#define PROTO_T_LOG         0x04u  /* 文字日誌（保留）   */
#define PROTO_T_INFO        0x05u  /* 裝置資訊（回應 GET_INFO） */
#define PROTO_T_BTN         0x06u  /* 按鍵回報（手柄模式；即時 press/release） */
#define PROTO_T_KEYMAP      0x07u  /* 按鍵映射內容（回應 GET_KEYMAP，每鍵一幀） */
#define PROTO_T_ACK         0x7Fu  /* 命令回覆           */

/* ---- 訊息型別：主機 → 裝置（bit7 = 1） ---- */
#define PROTO_T_PING        0x80u
#define PROTO_T_SET_RATE    0x81u  /* payload: u8 hz（0=停止回報） */
#define PROTO_T_RESET_CLICKS 0x82u
#define PROTO_T_CAL_GYRO    0x83u
#define PROTO_T_GET_INFO    0x84u
#define PROTO_T_SET_KEYMAP  0x85u  /* 寫入一鍵映射（EEPROM 持久化） */
#define PROTO_T_GET_KEYMAP  0x86u  /* 查詢全部映射 */

/* KEYMAP / SET_KEYMAP 共用 payload（變長，無 packed struct）：
 *   u8 key_id（2..5） | u8 len（0=未設定，上限 35） | char spec[len]
 * spec 為 ASCII 映射規格字串（"mouse:left"、"ctrl+c"…），
 * 語意由 PC 端解讀，裝置只負責保存 —— 裝置是「設定的載體」。 */
#define PROTO_KEYMAP_SPEC_MAX  35u

/* ---- ACK 狀態碼 ---- */
#define PROTO_ACK_OK        0u
#define PROTO_ACK_ERR       1u
#define PROTO_ACK_UNKNOWN   2u
#define PROTO_ACK_BUSY      3u

/* ---- 事件代碼（PROTO_T_EVENT.id） ---- */
#define PROTO_EV_BOOT           1u   /* arg = 重置原因旗標 */
#define PROTO_EV_BTN_CLICK      2u   /* arg = 累計次數     */
#define PROTO_EV_CLICKS_RESET   3u
#define PROTO_EV_CAL_DONE       4u   /* arg = 0 成功 / 1 失敗 */
#define PROTO_EV_BTN_DOUBLE     5u
#define PROTO_EV_AIRMOUSE       6u   /* arg = 1 開啟 / 0 關閉（KEY0 雙擊切換） */

/* ---- Payload 結構（packed，直接以記憶體映像傳輸；兩端皆為 LE） ---- */

typedef struct __attribute__((packed)) {
    int16_t  roll_cdeg;    /* 角度 × 100 */
    int16_t  pitch_cdeg;
    int16_t  yaw_cdeg;
    int16_t  temp_cdegc;   /* 溫度 × 100 */
    uint32_t click_count;
    uint32_t uptime_ms;
} proto_attitude_t;

typedef struct __attribute__((packed)) {
    uint8_t  cpu_percent;
    uint8_t  sys_flags;    /* bit0 imu_ok, bit1 oled_ok, bit2 stor_ok, bit3 bridge */
    uint16_t err_count;    /* i2c + uart + crc 錯誤總和（飽和） */
    uint32_t boot_count;
    uint32_t uptime_s;
} proto_sysstat_t;

typedef struct __attribute__((packed)) {
    uint8_t  id;
    uint32_t arg;
} proto_event_t;

/* 按鍵回報：action 直接沿用 mw/button.h 的 btn_event_t 數值
 * （0 press / 1 release / 2 click / 3 double / 4 long）。
 * 手柄映射主要吃 press/release（可表達「按住」語意），
 * click/double/long 一併上報供 PC 端進階應用。 */
typedef struct __attribute__((packed)) {
    uint8_t key_id;     /* 0..5（KEY0..KEY5） */
    uint8_t action;
} proto_btn_t;

typedef struct __attribute__((packed)) {
    uint8_t req_type;
    uint8_t req_seq;
    uint8_t status;
} proto_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t  proto_ver;
    char     fw_ver[11];   /* NUL 填充 */
    uint32_t uid[3];       /* MCU 96-bit 唯一 ID */
} proto_info_t;

_Static_assert(sizeof(proto_attitude_t) == 16, "attitude payload size");
_Static_assert(sizeof(proto_sysstat_t) == 12, "sysstat payload size");
_Static_assert(sizeof(proto_event_t) == 5, "event payload size");
_Static_assert(sizeof(proto_btn_t) == 2, "btn payload size");
_Static_assert(sizeof(proto_ack_t) == 3, "ack payload size");
_Static_assert(sizeof(proto_info_t) == 24, "info payload size");

/* ---- 解析器 ---- */

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    uint8_t payload[PROTO_MAX_PAYLOAD];
} proto_frame_t;

typedef void (*proto_frame_cb_t)(const proto_frame_t *frame, void *user);

typedef struct {
    uint32_t rx_frames;
    uint32_t crc_errors;
    uint32_t resyncs;      /* 掉出同步的次數（含 CRC 錯與非法長度） */
} proto_stats_t;

typedef struct {
    /* private */
    uint8_t  state;
    uint16_t idx;
    uint16_t crc_calc;
    uint16_t crc_rx;
    proto_frame_t frame;
    proto_frame_cb_t cb;
    void    *user;
    proto_stats_t stats;
} proto_parser_t;

void proto_parser_init(proto_parser_t *p, proto_frame_cb_t cb, void *user);

/** @brief 餵入串流位元組；每解出一個合法封包呼叫一次 cb */
void proto_parser_feed(proto_parser_t *p, const uint8_t *data, uint16_t len);

/**
 * @brief 組出完整封包。
 * @param out  輸出緩衝區（至少 len + PROTO_OVERHEAD）
 * @return 封包總長；0 表示參數錯誤 / 緩衝區不足
 */
uint16_t proto_build(uint8_t *out, uint16_t cap, uint8_t type, uint8_t seq,
                     const void *payload, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* MW_PROTO_H */
