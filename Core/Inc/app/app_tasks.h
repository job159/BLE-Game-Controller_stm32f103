/**
 * @file    app_tasks.h
 * @brief   應用任務與跨任務服務介面。
 *
 * 任務一覽（週期見 app_main.c 註冊處）：
 *   task_imu      10ms   姿態解算 + 校正請求處理
 *   task_btn      10ms   按鍵掃描與事件分派
 *   task_comm_rx  10ms   BLE 收包解析 / UART 橋接模式
 *   task_comm_tx  依設定  姿態遙測發送（預設 10Hz，可由命令調整）
 *   task_ui       100ms  OLED 頁面渲染 + 狀態 LED
 *   task_storage  250ms  EEPROM 延遲落盤
 *   task_sys      1s     系統狀態回報 + 看門狗簽到檢查
 *
 * 跨任務溝通原則：單一寫入者 + 明確的服務函式，不用全域旗標海。
 */
#ifndef APP_TASKS_H
#define APP_TASKS_H

#include "app/app_common.h"
#include "mw/proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 共享狀態（寫入者標注於欄位註解） ---- */
typedef struct {
    uint8_t  ui_page;          /* task_ui / task_btn        */
    bool     bridge_mode;      /* CLI 指令進入、comm 離開   */
    bool     cal_request;      /* 任何人設、task_imu 清     */
    bool     ui_test_mode;     /* CLI `oled test` 設、task_ui 讀（顯示測試圖） */
    bool     air_mouse;        /* KEY0 雙擊切換：PC 端據此把姿態轉為游標移動 */
    uint32_t comm_tx_frames;   /* task_comm_tx              */
} app_state_t;

app_state_t *app_state(void);

/* ---- 任務函式（由排程器呼叫） ---- */
void task_imu(void);
void task_btn(void);
void task_comm_rx(void);
void task_comm_tx(void);
void task_ui(void);
void task_storage(void);
void task_sys(void);

/* ---- 任務一次性初始化（app_main_init 呼叫） ---- */
void task_btn_init(void);
void task_comm_init(void);
void task_ui_init(void);

/* ---- comm 服務 ---- */
void comm_send(uint8_t type, const void *payload, uint8_t len);
void comm_send_event(uint8_t ev_id, uint32_t arg);
void comm_send_btn(uint8_t key_id, uint8_t action);
void comm_set_telemetry_hz(uint8_t hz);   /* 0 = 停止回報；同步持久化 */
uint8_t comm_telemetry_hz(void);
const proto_stats_t *comm_proto_stats(void);

/* ---- ui 服務 ---- */
void ui_next_page(void);
void ui_notify(const char *msg);          /* 底部提示條，顯示 1.5 秒 */

/* ---- CLI 命令表（app_cli.c 提供） ---- */
void app_cli_init(void);

/* ---- 排程任務 id（app_main.c 註冊後填入，動態調週期用） ---- */
extern int g_task_id_comm_tx;

#ifdef __cplusplus
}
#endif

#endif /* APP_TASKS_H */
