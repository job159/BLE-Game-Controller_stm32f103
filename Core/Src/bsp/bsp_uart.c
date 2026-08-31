/**
 * @file    bsp_uart.c
 * @brief   UART 服務實作。
 *
 * 中斷優先權設計（NVIC group 4，數字小 = 優先權高）：
 *   USART2 / DMA1_Ch6（BLE RX）    = 5  ── 同級互不搶佔，
 *   DMA1_Ch7（BLE TX）             = 6     回呼天然序列化，
 *   USART1（除錯）                 = 8     共享資料免加鎖。
 * 主迴圈與 ISR 之間則靠 SPSC ring buffer 的單向索引契約。
 */
#include "bsp/bsp_uart.h"
#include "bsp/bsp_board.h"
#include "mw/ringbuf.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- 緩衝區配置（皆為 2 的冪） ---- */
#define DBG_TX_BUF_SIZE   512u
#define DBG_RX_BUF_SIZE   256u
#define BLE_TX_BUF_SIZE   512u
#define BLE_RX_RING_SIZE  1024u
#define BLE_RX_DMA_SIZE   256u    /* DMA 環形硬體緩衝 */

typedef struct {
    UART_HandleTypeDef *h;
    ringbuf_t   tx_rb;
    ringbuf_t   rx_rb;
    volatile bool     tx_active;
    volatile uint16_t tx_chunk;    /* 本次 IT/DMA 傳送的長度 */
    bsp_uart_stats_t  stats;
} uart_port_t;

static uart_port_t s_dbg;
static uart_port_t s_ble;

static uint8_t s_dbg_tx_mem[DBG_TX_BUF_SIZE];
static uint8_t s_dbg_rx_mem[DBG_RX_BUF_SIZE];
static uint8_t s_ble_tx_mem[BLE_TX_BUF_SIZE];
static uint8_t s_ble_rx_mem[BLE_RX_RING_SIZE];

static uint8_t s_dbg_rx_byte;                    /* USART1 逐位元組接收 */
static uint8_t s_ble_dma_buf[BLE_RX_DMA_SIZE];   /* USART2 DMA 環形緩衝 */
static volatile uint16_t s_ble_dma_pos;          /* 已消費到的 DMA 位置 */

/* ------------------------------------------------------------------ */
/*                            內部工具                                 */
/* ------------------------------------------------------------------ */

static void dbg_rx_arm(void)
{
    (void)HAL_UART_Receive_IT(s_dbg.h, &s_dbg_rx_byte, 1u);
}

static void ble_rx_start(void)
{
    s_ble_dma_pos = 0u;
    s_ble.stats.rx_restarts++;
    (void)HAL_UARTEx_ReceiveToIdle_DMA(s_ble.h, s_ble_dma_buf, BLE_RX_DMA_SIZE);
}

/* 由主迴圈或 ISR 呼叫皆安全：以臨界區做 test-and-set 啟動傳送 */
static void port_kick_tx(uart_port_t *p, bool use_dma)
{
    uint32_t key = app_enter_critical();
    if (!p->tx_active) {
        const uint8_t *chunk;
        uint16_t n = rb_peek_linear(&p->tx_rb, &chunk);
        if (n > 0u) {
            p->tx_active = true;
            p->tx_chunk = n;
            HAL_StatusTypeDef st = use_dma
                ? HAL_UART_Transmit_DMA(p->h, (uint8_t *)chunk, n)
                : HAL_UART_Transmit_IT(p->h, (uint8_t *)chunk, n);
            if (st != HAL_OK) {
                p->tx_active = false;   /* 啟動失敗，留在 ring 裡下次再試 */
            }
        }
    }
    app_exit_critical(key);
}

static uint16_t port_write(uart_port_t *p, bool use_dma,
                           const void *data, uint16_t len)
{
    uint16_t queued = rb_put(&p->tx_rb, (const uint8_t *)data, len);
    p->stats.tx_bytes += queued;
    p->stats.tx_dropped = p->tx_rb.dropped;
    port_kick_tx(p, use_dma);
    return queued;
}

/* ------------------------------------------------------------------ */
/*                            公開 API                                 */
/* ------------------------------------------------------------------ */

void bsp_uart_init(void)
{
    s_dbg.h = BSP_UART_DBG_HANDLE;
    s_ble.h = BSP_UART_BLE_HANDLE;

    (void)rb_init(&s_dbg.tx_rb, s_dbg_tx_mem, DBG_TX_BUF_SIZE);
    (void)rb_init(&s_dbg.rx_rb, s_dbg_rx_mem, DBG_RX_BUF_SIZE);
    (void)rb_init(&s_ble.tx_rb, s_ble_tx_mem, BLE_TX_BUF_SIZE);
    (void)rb_init(&s_ble.rx_rb, s_ble_rx_mem, BLE_RX_RING_SIZE);

    dbg_rx_arm();
    ble_rx_start();
    s_dbg.stats.rx_restarts = 0u;   /* 首次啟動不算「重啟」 */
    s_ble.stats.rx_restarts = 0u;
}

uint16_t dbg_write(const void *data, uint16_t len)
{
    return port_write(&s_dbg, false, data, len);
}

void dbg_printf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;   /* 截斷過長訊息 */
    }
    (void)dbg_write(buf, (uint16_t)n);
}

int dbg_getc(void)
{
    return rb_getc(&s_dbg.rx_rb);
}

void dbg_flush(void)
{
    /* 限重置/致命錯誤路徑使用：最多等 200ms 將 ring 內容送完 */
    uint32_t deadline = HAL_GetTick() + 200u;
    while (!rb_is_empty(&s_dbg.tx_rb) || s_dbg.tx_active) {
        if (HAL_GetTick() >= deadline) {
            break;
        }
    }
}

const bsp_uart_stats_t *dbg_stats(void)
{
    return &s_dbg.stats;
}

uint16_t ble_write(const void *data, uint16_t len)
{
    return port_write(&s_ble, true, data, len);
}

uint16_t ble_read(void *data, uint16_t maxlen)
{
    uint16_t n = rb_get(&s_ble.rx_rb, (uint8_t *)data, maxlen);
    return n;
}

uint16_t ble_rx_used(void)
{
    return rb_used(&s_ble.rx_rb);
}

const bsp_uart_stats_t *ble_stats(void)
{
    return &s_ble.stats;
}

int ble_set_baud(uint32_t baud)
{
    if ((baud < 1200u) || (baud > 921600u)) {
        return APP_EINVAL;
    }
    /* 停掉進行中的收發（含 DMA），改鮑率後整組重啟 */
    (void)HAL_UART_Abort(s_ble.h);
    s_ble.h->Init.BaudRate = baud;
    if (HAL_UART_Init(s_ble.h) != HAL_OK) {
        return APP_ERR;
    }
    uint32_t key = app_enter_critical();
    s_ble.tx_active = false;
    s_ble.tx_chunk = 0u;
    app_exit_critical(key);
    rb_clear(&s_ble.tx_rb);   /* 舊鮑率下排隊的資料已無意義 */
    rb_clear(&s_ble.rx_rb);
    ble_rx_start();
    return APP_OK;
}

uint32_t ble_get_baud(void)
{
    return s_ble.h->Init.BaudRate;
}

/* ------------------------------------------------------------------ */
/*                     HAL 回呼（ISR context）                        */
/* ------------------------------------------------------------------ */

/** BLE RX：把 DMA 環形緩衝中 [s_ble_dma_pos, pos) 的新資料搬進 RX ring */
static void ble_drain_dma(uint16_t pos)
{
    if (pos == s_ble_dma_pos) {
        return;
    }
    if (pos > s_ble_dma_pos) {
        uint16_t n = (uint16_t)(pos - s_ble_dma_pos);
        rb_put(&s_ble.rx_rb, &s_ble_dma_buf[s_ble_dma_pos], n);
        s_ble.stats.rx_bytes += n;
    } else {
        /* DMA 已環繞：先搬尾段再搬頭段 */
        uint16_t tail_n = (uint16_t)(BLE_RX_DMA_SIZE - s_ble_dma_pos);
        rb_put(&s_ble.rx_rb, &s_ble_dma_buf[s_ble_dma_pos], tail_n);
        rb_put(&s_ble.rx_rb, &s_ble_dma_buf[0], pos);
        s_ble.stats.rx_bytes += (uint32_t)tail_n + pos;
    }
    s_ble.stats.rx_dropped = s_ble.rx_rb.dropped;
    s_ble_dma_pos = (pos >= BLE_RX_DMA_SIZE) ? 0u : pos;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == s_ble.h->Instance) {
        ble_drain_dma(Size);
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == s_dbg.h->Instance) {
        if (rb_putc(&s_dbg.rx_rb, s_dbg_rx_byte)) {
            s_dbg.stats.rx_bytes++;
        } else {
            s_dbg.stats.rx_dropped = s_dbg.rx_rb.dropped;
        }
        dbg_rx_arm();
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    uart_port_t *p = NULL;
    bool use_dma = false;
    if (huart->Instance == s_dbg.h->Instance) {
        p = &s_dbg;
    } else if (huart->Instance == s_ble.h->Instance) {
        p = &s_ble;
        use_dma = true;
    }
    if (p != NULL) {
        rb_skip(&p->tx_rb, p->tx_chunk);
        p->tx_chunk = 0u;
        p->tx_active = false;
        port_kick_tx(p, use_dma);   /* ring 還有資料就接著送 */
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == s_dbg.h->Instance) {
        s_dbg.stats.hw_errors++;
        dbg_rx_arm();               /* 錯誤已由 HAL 清旗標，重掛接收 */
    } else if (huart->Instance == s_ble.h->Instance) {
        s_ble.stats.hw_errors++;
        ble_rx_start();             /* DMA 接收引擎整組重啟 */
    }
}
