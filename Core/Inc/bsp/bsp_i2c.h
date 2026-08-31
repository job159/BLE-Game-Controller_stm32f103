/**
 * @file    bsp_i2c.h
 * @brief   I2C 匯流排抽象：以「匯流排代號」取代 HAL handle。
 *
 * 分工：
 *  - BSP_I2C_SENSOR（I2C1）：MPU6050 + AT24Cxx，阻塞式傳輸（單筆量小）。
 *  - BSP_I2C_DISP  （I2C2）：SSD1306，framebuffer 走 DMA 非阻塞刷新，
 *    顯示流量（1KB/幀）不會卡住感測與儲存。
 *
 * 穩定性設計：
 *  - 連續錯誤達門檻自動執行 bus recovery（SCL 手動打 9 個 clock 釋放
 *    被從機拉死的 SDA）——真實產品必備，否則熱插拔/雜訊會讓 I2C 永久卡死。
 */
#ifndef BSP_I2C_H
#define BSP_I2C_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_I2C_SENSOR = 0,   /* I2C1：MPU6050 + AT24Cxx */
    BSP_I2C_DISP   = 1,   /* I2C2：SSD1306 OLED      */
    BSP_I2C_BUS_COUNT
} bsp_i2c_bus_t;

typedef struct {
    uint32_t xfer_ok;
    uint32_t xfer_err;
    uint32_t recover_cnt;
} bsp_i2c_stats_t;

/**
 * @brief 對裝置的暫存器（記憶體位址）寫入。
 * @param addr7    7-bit 裝置位址
 * @param mem      暫存器 / 記憶體位址
 * @param mem_size 位址長度（1 或 2 bytes）
 * @return APP_OK / APP_ETIMEOUT / APP_ERR
 */
int bsp_i2c_mem_write(bsp_i2c_bus_t bus, uint8_t addr7, uint16_t mem,
                      uint8_t mem_size, const uint8_t *data, uint16_t len,
                      uint32_t timeout_ms);

int bsp_i2c_mem_read(bsp_i2c_bus_t bus, uint8_t addr7, uint16_t mem,
                     uint8_t mem_size, uint8_t *data, uint16_t len,
                     uint32_t timeout_ms);

/** @brief 探測裝置是否 ACK（EEPROM ack-polling、開機裝置盤點用） */
int bsp_i2c_probe(bsp_i2c_bus_t bus, uint8_t addr7, uint32_t timeout_ms);

/**
 * @brief 非阻塞串流寫入：prefix（1 byte，如 SSD1306 的 0x40 資料前導碼）
 *        後接 len bytes 資料，由 DMA 搬運。
 * @note  data 緩衝區在傳輸完成前不可修改。以 bsp_i2c_dma_busy() 查詢進度。
 * @return APP_OK 已啟動 / APP_EBUSY 前一筆未完成 / APP_ERR
 */
int bsp_i2c_write_stream_dma(bsp_i2c_bus_t bus, uint8_t addr7, uint8_t prefix,
                             const uint8_t *data, uint16_t len);

bool bsp_i2c_dma_busy(bsp_i2c_bus_t bus);

/** @brief 手動觸發匯流排復原（9 個 SCL clock + STOP，之後重新初始化 HAL） */
int bsp_i2c_recover(bsp_i2c_bus_t bus);

const bsp_i2c_stats_t *bsp_i2c_stats(bsp_i2c_bus_t bus);

#ifdef __cplusplus
}
#endif

#endif /* BSP_I2C_H */
