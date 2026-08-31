/**
 * @file    storage.c
 * @brief   雙槽儲存層實作。
 *
 * 槽格式（32B）：
 *   ┌───────┬───────┬─────┬─────┬─────────────┬──────────┐
 *   │ MAGIC │  SEQ  │ LEN │ RSV │   PAYLOAD   │ CRC16 LE │
 *   │  u16  │  u16  │ u8  │ u8  │  LEN bytes  │   u16    │
 *   └───────┴───────┴─────┴─────┴─────────────┴──────────┘
 *   CRC 涵蓋 SEQ..PAYLOAD。SEQ 單調遞增（u16 環繞），
 *   兩槽都有效時取 SEQ 較新者（有號差值比較，環繞安全）。
 *
 * 上線策略（穩定性重點）：
 *   開機探測失敗「不會」永久停用持久化 —— F103 上電瞬間 I2C 匯流排
 *   可能殘留 BUSY、EEPROM 供電也可能較慢。除了開機重試 3 次外，
 *   離線狀態下每 5 秒自動重試；遲來上線時把 EEPROM 歷史與本會期的
 *   RAM 變更合併後立即落盤，兩邊的資料都不丟。
 */
#include "mw/storage.h"
#include "mw/crc16.h"
#include "app/app_config.h"
#include "drivers/drv_at24cxx.h"
#include "stm32f1xx_hal.h"
#include <string.h>

#define STOR_MAGIC        0xA5C3u
#define STOR_SLOT_SIZE    32u
#define STOR_HDR_SIZE     6u                     /* magic+seq+len+rsv */
#define STOR_PAYLOAD_MAX  (STOR_SLOT_SIZE - STOR_HDR_SIZE - 2u)
#define STOR_SLOT_A_ADDR  0x00u
#define STOR_SLOT_B_ADDR  0x20u
#define STOR_REPROBE_MS   5000u                  /* 離線重試上線週期 */

_Static_assert(sizeof(stor_record_t) <= STOR_PAYLOAD_MAX,
               "stor_record_t 超出槽容量，請縮減欄位或加大 STOR_SLOT_SIZE");

static stor_record_t s_cache;
static stor_stats_t  s_stats;
static uint16_t s_seq;
static uint8_t  s_next_slot;      /* 下次寫入的槽（0=A 1=B） */
static bool     s_online;         /* EEPROM 是否在線 */
static bool     s_dirty;
static bool     s_touched;        /* 本會期是否有過使用者變更（合併判斷用） */
static uint32_t s_dirty_since;    /* 首次變髒時刻 */
static uint32_t s_last_change;    /* 最近變更時刻 */
static uint32_t s_last_reprobe;   /* 離線重試上線的節流 */

/* ---- 手柄映射區（每鍵獨立條目，各自帶 CRC） ----
 * 位址配置（AT24C02 256B）：0x40 + idx*40，共 4 條 = 0x40..0xDF。
 * 條目：MAGIC(1) key_id(1) len(1) spec(35, 零填充) CRC16(2)
 * CRC 涵蓋 key_id..spec。單條寫入約 5 頁 ≈ 30ms，由 stor_poll
 * 每 tick 最多寫一條分散負載。 */
#define KM_ADDR        0x40u
#define KM_ENTRY_SIZE  40u
#define KM_MAGIC       0x4Du

static char    s_km_spec[STOR_KEYMAP_KEYS][STOR_KEYMAP_SPEC_MAX + 1u];
static uint8_t s_km_len[STOR_KEYMAP_KEYS];
static uint8_t s_km_dirty;       /* bit per entry */
static bool    s_km_region_ok;   /* EEPROM 容量足夠放映射區 */

static uint16_t slot_addr(uint8_t slot)
{
    return (slot == 0u) ? STOR_SLOT_A_ADDR : STOR_SLOT_B_ADDR;
}

static uint16_t km_entry_addr(uint8_t idx)
{
    return (uint16_t)(KM_ADDR + (uint16_t)idx * KM_ENTRY_SIZE);
}

static void km_build_raw(uint8_t idx, uint8_t raw[KM_ENTRY_SIZE])
{
    memset(raw, 0, KM_ENTRY_SIZE);
    raw[0] = KM_MAGIC;
    raw[1] = (uint8_t)(STOR_KEYMAP_FIRST_KEY + idx);
    raw[2] = s_km_len[idx];
    memcpy(&raw[3], s_km_spec[idx], s_km_len[idx]);
    uint16_t crc = crc16_ccitt(&raw[1], 37u, 0xFFFFu);
    raw[38] = (uint8_t)(crc & 0xFFu);
    raw[39] = (uint8_t)(crc >> 8);
}

static int km_entry_store(uint8_t idx)
{
    uint8_t raw[KM_ENTRY_SIZE];
    km_build_raw(idx, raw);
    int rc = at24_write(km_entry_addr(idx), raw, sizeof(raw));
    if (rc != APP_OK) {
        return rc;
    }
    uint8_t verify[KM_ENTRY_SIZE];
    rc = at24_read(km_entry_addr(idx), verify, sizeof(verify));
    if ((rc != APP_OK) || (memcmp(raw, verify, sizeof(raw)) != 0)) {
        return APP_ECRC;
    }
    return APP_OK;
}

/* 自 EEPROM 載入全部條目；RAM 中已標髒（離線期間的變更）者跳過不覆蓋 */
static void km_load_all(void)
{
    if (!s_km_region_ok) {
        return;
    }
    for (uint8_t idx = 0u; idx < STOR_KEYMAP_KEYS; idx++) {
        if (s_km_dirty & (1u << idx)) {
            continue;
        }
        uint8_t raw[KM_ENTRY_SIZE];
        if (at24_read(km_entry_addr(idx), raw, sizeof(raw)) != APP_OK) {
            continue;
        }
        uint16_t crc = (uint16_t)(raw[38] | ((uint16_t)raw[39] << 8));
        if ((raw[0] != KM_MAGIC) ||
            (raw[1] != STOR_KEYMAP_FIRST_KEY + idx) ||
            (raw[2] > STOR_KEYMAP_SPEC_MAX) ||
            (crc != crc16_ccitt(&raw[1], 37u, 0xFFFFu))) {
            s_km_len[idx] = 0u;   /* 空白/損毀 → 視為未設定 */
            continue;
        }
        s_km_len[idx] = raw[2];
        memcpy(s_km_spec[idx], &raw[3], raw[2]);
        s_km_spec[idx][raw[2]] = '\0';
    }
}

static void apply_defaults(void)
{
    memset(&s_cache, 0, sizeof(s_cache));
    s_cache.telemetry_hz = APP_TELEMETRY_HZ_DEF;
}

/**
 * @brief 讀取並驗證一個槽。
 * @param[out] rec  驗證通過時輸出記錄（舊短記錄自動零擴充）
 * @param[out] seq  記錄序號
 * @return APP_OK / APP_ECRC（含 magic 不符）/ 傳輸錯誤碼
 */
static int slot_load(uint8_t slot, stor_record_t *rec, uint16_t *seq)
{
    uint8_t raw[STOR_SLOT_SIZE];
    int rc = at24_read(slot_addr(slot), raw, sizeof(raw));
    if (rc != APP_OK) {
        return rc;
    }

    uint16_t magic = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));
    uint8_t  len   = raw[4];
    if ((magic != STOR_MAGIC) || (len == 0u) || (len > STOR_PAYLOAD_MAX)) {
        return APP_ECRC;
    }
    uint16_t crc_calc = crc16_ccitt(&raw[2], (uint16_t)(4u + len), 0xFFFFu);
    uint16_t crc_read = (uint16_t)(raw[STOR_HDR_SIZE + len] |
                        ((uint16_t)raw[STOR_HDR_SIZE + len + 1u] << 8));
    if (crc_calc != crc_read) {
        return APP_ECRC;
    }

    /* 版本遷移：舊韌體記錄較短 → 其餘欄位補零 */
    memset(rec, 0, sizeof(*rec));
    memcpy(rec, &raw[STOR_HDR_SIZE], APP_MIN(len, (uint8_t)sizeof(*rec)));
    *seq = (uint16_t)(raw[2] | ((uint16_t)raw[3] << 8));
    return APP_OK;
}

static int slot_store(uint8_t slot, const stor_record_t *rec, uint16_t seq)
{
    uint8_t raw[STOR_SLOT_SIZE];
    const uint8_t len = (uint8_t)sizeof(*rec);

    memset(raw, 0xFF, sizeof(raw));
    raw[0] = (uint8_t)(STOR_MAGIC & 0xFFu);
    raw[1] = (uint8_t)(STOR_MAGIC >> 8);
    raw[2] = (uint8_t)(seq & 0xFFu);
    raw[3] = (uint8_t)(seq >> 8);
    raw[4] = len;
    raw[5] = 0u;
    memcpy(&raw[STOR_HDR_SIZE], rec, len);
    uint16_t crc = crc16_ccitt(&raw[2], (uint16_t)(4u + len), 0xFFFFu);
    raw[STOR_HDR_SIZE + len]      = (uint8_t)(crc & 0xFFu);
    raw[STOR_HDR_SIZE + len + 1u] = (uint8_t)(crc >> 8);

    int rc = at24_write(slot_addr(slot), raw, sizeof(raw));
    if (rc != APP_OK) {
        return rc;
    }

    /* 讀回驗證：EEPROM 寫入是類比過程，驗證是便宜的保險 */
    stor_record_t verify;
    uint16_t vseq;
    rc = slot_load(slot, &verify, &vseq);
    if ((rc != APP_OK) || (vseq != seq) ||
        (memcmp(&verify, rec, sizeof(*rec)) != 0)) {
        return APP_ECRC;
    }
    return APP_OK;
}

/**
 * @brief 讀兩槽、挑最新有效記錄載入狀態（cache/seq/next_slot/來源）。
 * @return APP_OK；兩槽皆無效回 APP_ERR（狀態不變）
 */
static int load_best_into_state(void)
{
    stor_record_t rec_a, rec_b;
    uint16_t seq_a = 0u, seq_b = 0u;
    bool ok_a = (slot_load(0u, &rec_a, &seq_a) == APP_OK);
    bool ok_b = (slot_load(1u, &rec_b, &seq_b) == APP_OK);

    if (ok_a && ok_b) {
        /* 有號差值比較：SEQ 環繞後仍能判斷新舊 */
        if ((int16_t)(seq_a - seq_b) >= 0) {
            s_cache = rec_a; s_seq = seq_a; s_next_slot = 1u;
            s_stats.load_source = 1u;
        } else {
            s_cache = rec_b; s_seq = seq_b; s_next_slot = 0u;
            s_stats.load_source = 2u;
        }
    } else if (ok_a) {
        s_cache = rec_a; s_seq = seq_a; s_next_slot = 1u;
        s_stats.load_source = 1u;
    } else if (ok_b) {
        s_cache = rec_b; s_seq = seq_b; s_next_slot = 0u;
        s_stats.load_source = 2u;
    } else {
        return APP_ERR;
    }
    return APP_OK;
}

int stor_init(void)
{
    apply_defaults();
    s_seq = 0u;
    s_next_slot = 0u;
    s_dirty = false;
    s_touched = false;
    s_stats.load_source = 0u;
    s_last_reprobe = HAL_GetTick();
    memset(s_km_len, 0, sizeof(s_km_len));
    s_km_dirty = 0u;
    s_km_region_ok = false;   /* 探測後依「實際幾何」重新計算 */

    /* 上電探測重試：匯流排/EEPROM 就緒時間各有快慢 */
    s_online = false;
    for (int i = 0; i < 3; i++) {
        if (at24_probe() == APP_OK) {
            s_online = true;
            break;
        }
        HAL_Delay(10);
    }
    if (!s_online) {
        return APP_ENODEV;   /* stor_poll 會持續嘗試上線 */
    }

    /* 幾何可能被 at24 自檢修正（如 C02 誤設實為 C32），此刻的
     * at24_size() 才是事實 */
    s_km_region_ok =
        (at24_size() >= (KM_ADDR + STOR_KEYMAP_KEYS * KM_ENTRY_SIZE));
    km_load_all();
    return (load_best_into_state() == APP_OK) ? APP_OK : APP_ERR;
}

stor_record_t *stor_get(void)
{
    return &s_cache;
}

void stor_mark_dirty(void)
{
    uint32_t now = HAL_GetTick();
    if (!s_dirty) {
        s_dirty_since = now;
    }
    s_dirty = true;
    s_touched = true;
    s_last_change = now;
}

int stor_commit_now(void)
{
    if (!s_online) {
        return APP_ENODEV;
    }
    uint16_t new_seq = (uint16_t)(s_seq + 1u);
    int rc = slot_store(s_next_slot, &s_cache, new_seq);
    if (rc != APP_OK) {
        /* 該槽可能損壞：換槽再試一次 */
        s_stats.commit_fails++;
        s_next_slot ^= 1u;
        rc = slot_store(s_next_slot, &s_cache, new_seq);
        if (rc != APP_OK) {
            s_stats.commit_fails++;
            return rc;
        }
    }
    s_seq = new_seq;
    s_next_slot ^= 1u;    /* 下次寫另一槽 */
    s_dirty = false;
    s_stats.commits++;
    return APP_OK;
}

/* 遲來上線：合併「EEPROM 歷史記錄」與「本會期 RAM 變更」後落盤。
 * 開機那次的 boot_count++ 已加在 RAM（值為歷史缺席下的 1），
 * 因此合併規則：
 *   - 本會期無使用者變更 → 直接採用歷史記錄，再補記本次開機。
 *   - 有變更（例如已按了幾次 KEY0）→ 保留 RAM 的使用者資料，
 *     開機數 = 歷史值 + 本會期值；校正值若 RAM 沒有則承接歷史。 */
static void late_online_merge(void)
{
    stor_record_t ram = s_cache;
    bool touched = s_touched;

    if (load_best_into_state() == APP_OK) {
        if (touched) {
            stor_record_t merged = ram;
            merged.boot_count = s_cache.boot_count + ram.boot_count;
            if (!(merged.flags & STOR_FLAG_GYRO_CAL) &&
                (s_cache.flags & STOR_FLAG_GYRO_CAL)) {
                memcpy(merged.gyro_bias, s_cache.gyro_bias,
                       sizeof(merged.gyro_bias));
                merged.flags |= STOR_FLAG_GYRO_CAL;
            }
            s_cache = merged;
        } else {
            s_cache.boot_count++;
        }
    } else {
        s_cache = ram;   /* EEPROM 空白：RAM 現況成為第一筆記錄 */
    }
    km_load_all();       /* 離線期間的變更帶髒旗標，不會被覆蓋 */
    (void)stor_commit_now();
}

void stor_poll(void)
{
    uint32_t now = HAL_GetTick();

    if (!s_online) {
        /* 離線自癒：每 5 秒重試。一次開機探測失敗不該永久停用持久化 */
        if ((uint32_t)(now - s_last_reprobe) < STOR_REPROBE_MS) {
            return;
        }
        s_last_reprobe = now;
        if (at24_probe() != APP_OK) {
            return;
        }
        s_online = true;
        late_online_merge();
        return;
    }

    /* 手柄映射落盤：每次 poll 最多寫一條，分散 EEPROM 頁寫入的阻塞 */
    if (s_km_region_ok && (s_km_dirty != 0u)) {
        for (uint8_t idx = 0u; idx < STOR_KEYMAP_KEYS; idx++) {
            if (s_km_dirty & (1u << idx)) {
                if (km_entry_store(idx) == APP_OK) {
                    s_km_dirty &= (uint8_t)~(1u << idx);
                    s_stats.commits++;
                } else {
                    s_stats.commit_fails++;
                }
                break;   /* 本 tick 只處理一條 */
            }
        }
    }

    if (!s_dirty) {
        return;
    }
    bool idle_expired  = (uint32_t)(now - s_last_change) >= APP_STOR_COMMIT_DELAY_MS;
    bool force_expired = (uint32_t)(now - s_dirty_since) >= APP_STOR_COMMIT_FORCE_MS;
    if (idle_expired || force_expired) {
        (void)stor_commit_now();
    }
}

bool stor_healthy(void)
{
    return s_online;
}

const stor_stats_t *stor_stats(void)
{
    return &s_stats;
}

bool stor_is_dirty(void)
{
    return s_dirty;
}

uint16_t stor_seq(void)
{
    return s_seq;
}

uint8_t stor_active_slot(void)
{
    return s_next_slot ^ 1u;
}

int stor_keymap_get(uint8_t key_id, char *buf, uint8_t cap)
{
    if ((key_id < STOR_KEYMAP_FIRST_KEY) ||
        (key_id >= STOR_KEYMAP_FIRST_KEY + STOR_KEYMAP_KEYS) ||
        (buf == NULL) || (cap == 0u)) {
        return APP_EINVAL;
    }
    uint8_t idx = (uint8_t)(key_id - STOR_KEYMAP_FIRST_KEY);
    uint8_t n = APP_MIN(s_km_len[idx], (uint8_t)(cap - 1u));
    memcpy(buf, s_km_spec[idx], n);
    buf[n] = '\0';
    return (int)n;
}

int stor_keymap_set(uint8_t key_id, const char *spec, uint8_t len)
{
    if ((key_id < STOR_KEYMAP_FIRST_KEY) ||
        (key_id >= STOR_KEYMAP_FIRST_KEY + STOR_KEYMAP_KEYS) ||
        (len > STOR_KEYMAP_SPEC_MAX) || ((spec == NULL) && (len > 0u))) {
        return APP_EINVAL;
    }
    if (!s_km_region_ok) {
        return APP_ENOSPACE;
    }
    uint8_t idx = (uint8_t)(key_id - STOR_KEYMAP_FIRST_KEY);
    if ((len == s_km_len[idx]) &&
        ((len == 0u) || (memcmp(spec, s_km_spec[idx], len) == 0))) {
        return APP_OK;   /* 內容相同：不寫，省磨耗 */
    }
    s_km_len[idx] = len;
    if (len > 0u) {
        memcpy(s_km_spec[idx], spec, len);
    }
    s_km_spec[idx][len] = '\0';
    s_km_dirty |= (uint8_t)(1u << idx);   /* stor_poll 落盤 */
    return APP_OK;
}
