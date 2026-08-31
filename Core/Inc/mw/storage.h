/**
 * @file    storage.h
 * @brief   參數持久化層：EEPROM 雙槽（A/B）輪替記錄。
 *
 * 為什麼不直接把變數寫進 EEPROM 固定位址（教學重點）？
 *  1. 斷電安全：寫到一半斷電 → 資料半新半舊且無從得知。
 *     雙槽輪替 + CRC：新記錄寫入「另一槽」，驗證通過前舊記錄完好，
 *     任何時刻至少有一份完整資料。
 *  2. 磨耗控制：EEPROM 每 cell 約 100 萬次寫入。
 *     - 變更先落在 RAM 快取，靜止 2 秒才 commit（連按 20 下只寫 1 次）；
 *     - 雙槽輪替讓磨耗平均分散到兩個位址。
 *  3. 版本遷移：記錄帶長度欄位，韌體升級後欄位增加時，
 *     舊記錄自動零擴充載入，不會整包作廢。
 *
 * EEPROM 配置（AT24C02, 256B）：
 *   0x00~0x1F  Slot A（32B）
 *   0x20~0x3F  Slot B（32B）
 *   0x40~      保留（實驗用，CLI `dump` 可檢視）
 */
#ifndef MW_STORAGE_H
#define MW_STORAGE_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 應用參數記錄（新增欄位只能「附加在尾端」以維持版本相容） */
typedef struct __attribute__((packed)) {
    uint32_t click_count;      /* KEY0 累計點擊次數 */
    uint32_t boot_count;       /* 開機次數 */
    int16_t  gyro_bias[3];     /* 陀螺儀零偏（raw LSB） */
    uint8_t  telemetry_hz;     /* BLE 姿態回報頻率 */
    uint8_t  flags;            /* bit0: gyro_bias 有效；bit1: BLE 模組已佈建 */
} stor_record_t;

#define STOR_FLAG_GYRO_CAL   (1u << 0)
#define STOR_FLAG_BLE_PROV   (1u << 1)   /* 模組鮑率已對齊（跳過開機探測） */

typedef struct {
    uint32_t commits;
    uint32_t commit_fails;
    uint32_t load_source;      /* 0=預設值 1=SlotA 2=SlotB */
} stor_stats_t;

/**
 * @brief 載入記錄（開機時呼叫一次）。
 * @return APP_OK=載入既有記錄；APP_ERR=無有效記錄，已套用預設值；
 *         APP_ENODEV=EEPROM 不在線（以預設值繼續，degraded mode）
 */
int stor_init(void);

/** @return RAM 快取的記錄（直接修改後呼叫 stor_mark_dirty） */
stor_record_t *stor_get(void);

/** @brief 標記已變更，storage 會在靜止期後自動落盤 */
void stor_mark_dirty(void);

/** @brief 週期輪詢（250ms 一次），執行延遲 commit 策略 */
void stor_poll(void);

/** @brief 立即落盤（重置前、關鍵事件用） */
int stor_commit_now(void);

bool stor_healthy(void);
const stor_stats_t *stor_stats(void);

/* ---- 檢視用查詢（OLED 儲存頁 / CLI 顯示） ---- */
bool stor_is_dirty(void);        /* true = RAM 快取有變更尚未落盤 */
uint16_t stor_seq(void);         /* 目前記錄序號（0 = 尚無記錄） */
uint8_t stor_active_slot(void);  /* 目前有效記錄所在槽：0=A 1=B */

/* ---- 手柄映射持久化（EEPROM 0x40 起，每鍵獨立 40B 條目+CRC） ----
 * 映射規格字串由 PC 端定義語意，裝置僅保存 —— 手柄自帶設定，
 * 換電腦連上即恢復。寫入走 stor_poll 分散排程（每 tick 最多一條），
 * 避免 comm 任務被 EEPROM 頁寫入卡住。 */
#define STOR_KEYMAP_FIRST_KEY  2u
#define STOR_KEYMAP_KEYS       4u
#define STOR_KEYMAP_SPEC_MAX   35u

/**
 * @brief 讀取一鍵的映射規格（RAM 快取，開機自 EEPROM 載入）。
 * @return 字串長度（0 = 未設定）；key_id 非 2..5 回 APP_EINVAL
 */
int stor_keymap_get(uint8_t key_id, char *buf, uint8_t cap);

/**
 * @brief 設定一鍵映射（len 0 = 清除）。內容相同時直接回 OK 不寫入。
 * @return APP_OK（已入 RAM，稍後落盤）/ APP_EINVAL / APP_ENOSPACE（EEPROM 太小）
 */
int stor_keymap_set(uint8_t key_id, const char *spec, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* MW_STORAGE_H */
