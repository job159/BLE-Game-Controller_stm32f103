/**
 * @file    crc16.c
 * @brief   CRC16-CCITT-FALSE 實作（位元展開版；無查表，換取零 flash 表格）。
 *
 * 取捨說明：查表法快 8 倍但吃 512B flash。此專案封包量小（<2KB/s），
 * 位元法在 72MHz 下綽綽有餘 —— 嵌入式的最佳化永遠先看「夠不夠」。
 */
#include "mw/crc16.h"

uint16_t crc16_ccitt_update(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte << 8;
    for (int i = 0; i < 8; i++) {
        if (crc & 0x8000u) {
            crc = (uint16_t)((crc << 1) ^ 0x1021u);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t crc16_ccitt(const uint8_t *data, uint16_t len, uint16_t seed)
{
    uint16_t crc = seed;
    for (uint16_t i = 0; i < len; i++) {
        crc = crc16_ccitt_update(crc, data[i]);
    }
    return crc;
}
