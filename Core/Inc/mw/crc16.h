/**
 * @file    crc16.h
 * @brief   CRC16-CCITT-FALSE（poly 0x1021, init 0xFFFF, 不反轉, 無 XOR-out）。
 *
 * 專案內所有完整性校驗共用同一種 CRC：
 *  - BLE 二進位協定框架（proto.c 與 tools/pyhost/protocol.py 必須一致）
 *  - EEPROM 儲存記錄（storage.c）
 * 驗證向量："123456789" → 0x29B1（tests/host 有單元測試）。
 */
#ifndef MW_CRC16_H
#define MW_CRC16_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 單一位元組累加（串流計算用） */
uint16_t crc16_ccitt_update(uint16_t crc, uint8_t byte);

/** @brief 一次計算整塊資料。seed 首次呼叫傳 0xFFFF；分段計算時傳前段結果 */
uint16_t crc16_ccitt(const uint8_t *data, uint16_t len, uint16_t seed);

#ifdef __cplusplus
}
#endif

#endif /* MW_CRC16_H */
