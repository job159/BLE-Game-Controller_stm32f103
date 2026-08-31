/**
 * @file    bsp_uart.h
 * @brief   UART 服務：除錯埠（USART1）與 BLE 埠（USART2）。
 *
 * 架構（教學重點）：
 *
 *   [應用層]      dbg_printf / cli          proto 封包
 *                      │                        │
 *   [本模組]   TX ring ─ IT 傳送        TX ring ─ DMA 傳送
 *              RX ring ─ 逐位元組 IT    RX ring ─ DMA 環形 + IDLE line
 *                      │                        │
 *   [硬體]         USART1 115200            USART2 115200 ↔ nRF52832
 *
 *  - 所有 API 非阻塞：寫入只是進 ring，由中斷/DMA 背景送出。
 *    緩衝區滿時「丟棄並計數」而非阻塞 —— 日誌絕不能拖垮即時任務，
 *    丟棄計數（stats.tx_dropped）讓問題可觀測。
 *  - BLE 收包用「DMA 環形緩衝 + IDLE 中斷」：這是 STM32 上高效
 *    接收不定長資料的標準解法，CPU 介入次數與封包數成正比而非位元組數。
 */
#ifndef BSP_UART_H
#define BSP_UART_H

#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t tx_dropped;    /* TX ring 滿而丟棄 */
    uint32_t rx_dropped;    /* RX ring 滿而丟棄 */
    uint32_t hw_errors;     /* ORE/FE/NE/PE 等硬體錯誤 */
    uint32_t rx_restarts;   /* RX 引擎重啟次數 */
} bsp_uart_stats_t;

/** @brief 初始化兩埠的收發引擎（於 MX_USARTx_Init 之後呼叫） */
void bsp_uart_init(void);

/* ---- 除錯埠（USART1）：僅限主迴圈 context 使用 ---- */
uint16_t dbg_write(const void *data, uint16_t len);
void     dbg_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int      dbg_getc(void);                 /* -1 = 無資料 */
void     dbg_flush(void);                /* 阻塞送完（限重置前使用） */
const bsp_uart_stats_t *dbg_stats(void);

/* ---- BLE 埠（USART2）：僅限主迴圈 context 使用 ---- */
uint16_t ble_write(const void *data, uint16_t len);
uint16_t ble_read(void *data, uint16_t maxlen);
uint16_t ble_rx_used(void);
const bsp_uart_stats_t *ble_stats(void);

/**
 * @brief 執行期切換 USART2 鮑率（清空收發佇列並重啟 RX 引擎）。
 *
 * 用途：對出廠鮑率非 115200 的透傳模組（如 HC-42 預設 9600）做
 * 現場佈建 —— `ble 9600` → `bridge` 下 AT 命令改模組 → `ble 115200`。
 * 僅影響本次開機（重開機回到 CubeMX 設定值）。
 */
int ble_set_baud(uint32_t baud);

/** @return 目前 USART2 鮑率 */
uint32_t ble_get_baud(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_UART_H */
