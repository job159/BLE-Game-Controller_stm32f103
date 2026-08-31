/**
 * @file    drv_at24cxx.h
 * @brief   AT24Cxx 系列 I2C EEPROM 驅動。
 *
 * 涵蓋三種定址方式（由 app_config.h 的 APP_AT24_TYPE 決定）：
 *  - AT24C01/02        ：1-byte 記憶體位址
 *  - AT24C04/08/16     ：1-byte 位址 + 高位址位元借用「裝置位址」低位
 *  - AT24C32/64        ：2-byte 記憶體位址
 *
 * 寫入語意（教學重點）：
 *  - EEPROM 只能整頁內連續寫（頁大小 8~32B），跨頁必須拆筆。
 *  - 每筆寫入後內部燒錄約需 5ms，期間裝置不 ACK；
 *    驅動以「ACK polling」等待完成，而非死等固定延遲。
 */
#ifndef DRV_AT24CXX_H
#define DRV_AT24CXX_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 探測 EEPROM 是否在線。
 * @note  自動掃描 0x50~0x57（A2A1A0 焊法各異的模組都能找到），
 *        掃到的位址之後供讀寫使用，可用 at24_dev_addr() 查詢。
 */
int at24_probe(void);

/** @return 掃描到的 7-bit 裝置位址；0 = 尚未找到 */
uint8_t at24_dev_addr(void);

/** @return 總容量（bytes），依 APP_AT24_TYPE */
uint16_t at24_size(void);

/** @brief 任意位址/長度讀取（驅動自行處理跨區塊） */
int at24_read(uint16_t addr, uint8_t *buf, uint16_t len);

/** @brief 任意位址/長度寫入（驅動自行處理跨頁與 ACK polling） */
int at24_write(uint16_t addr, const uint8_t *buf, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* DRV_AT24CXX_H */
