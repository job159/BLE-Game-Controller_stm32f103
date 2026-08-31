/**
 * @file    bsp_i2c.c
 * @brief   I2C 匯流排抽象實作：阻塞傳輸、DMA 串流、錯誤統計與匯流排復原。
 */
#include "bsp/bsp_i2c.h"
#include "bsp/bsp_board.h"
#include "bsp/bsp.h"

#define I2C_CONSEC_ERR_RECOVER   3   /* 連續錯誤達此數自動 bus recovery */

typedef struct {
    I2C_HandleTypeDef *h;
    GPIO_TypeDef      *scl_port;
    uint16_t           scl_pin;
    GPIO_TypeDef      *sda_port;
    uint16_t           sda_pin;
    volatile bool      dma_busy;
    uint8_t            consec_err;
    bsp_i2c_stats_t    stats;
} i2c_ctx_t;

static i2c_ctx_t s_bus[BSP_I2C_BUS_COUNT] = {
    [BSP_I2C_SENSOR] = {
        .h = NULL,  /* runtime 決定，避免全域初始化順序問題 */
        .scl_port = BSP_I2C1_SCL_PORT, .scl_pin = BSP_I2C1_SCL_PIN,
        .sda_port = BSP_I2C1_SDA_PORT, .sda_pin = BSP_I2C1_SDA_PIN,
    },
    [BSP_I2C_DISP] = {
        .h = NULL,
        .scl_port = BSP_I2C2_SCL_PORT, .scl_pin = BSP_I2C2_SCL_PIN,
        .sda_port = BSP_I2C2_SDA_PORT, .sda_pin = BSP_I2C2_SDA_PIN,
    },
};

static i2c_ctx_t *ctx_of(bsp_i2c_bus_t bus)
{
    if (bus >= BSP_I2C_BUS_COUNT) {
        return NULL;
    }
    i2c_ctx_t *c = &s_bus[bus];
    if (c->h == NULL) {
        c->h = (bus == BSP_I2C_SENSOR) ? BSP_I2C_SENSOR_HANDLE : BSP_I2C_DISP_HANDLE;
    }
    return c;
}

static int map_hal(HAL_StatusTypeDef st)
{
    switch (st) {
    case HAL_OK:      return APP_OK;
    case HAL_BUSY:    return APP_EBUSY;
    case HAL_TIMEOUT: return APP_ETIMEOUT;
    default:          return APP_ERR;
    }
}

/* 集中處理結果：成功清除連錯計數；失敗累計並於達門檻時嘗試復原 */
static int finish(i2c_ctx_t *c, bsp_i2c_bus_t bus, HAL_StatusTypeDef st)
{
    if (st == HAL_OK) {
        c->consec_err = 0u;
        c->stats.xfer_ok++;
        return APP_OK;
    }
    c->stats.xfer_err++;
    if (++c->consec_err >= I2C_CONSEC_ERR_RECOVER) {
        (void)bsp_i2c_recover(bus);
        c->consec_err = 0u;
    }
    return map_hal(st);
}

int bsp_i2c_mem_write(bsp_i2c_bus_t bus, uint8_t addr7, uint16_t mem,
                      uint8_t mem_size, const uint8_t *data, uint16_t len,
                      uint32_t timeout_ms)
{
    i2c_ctx_t *c = ctx_of(bus);
    if ((c == NULL) || (data == NULL) || ((mem_size != 1u) && (mem_size != 2u))) {
        return APP_EINVAL;
    }
    HAL_StatusTypeDef st = HAL_I2C_Mem_Write(c->h, (uint16_t)(addr7 << 1), mem,
                                             (mem_size == 1u) ? I2C_MEMADD_SIZE_8BIT
                                                              : I2C_MEMADD_SIZE_16BIT,
                                             (uint8_t *)data, len, timeout_ms);
    return finish(c, bus, st);
}

int bsp_i2c_mem_read(bsp_i2c_bus_t bus, uint8_t addr7, uint16_t mem,
                     uint8_t mem_size, uint8_t *data, uint16_t len,
                     uint32_t timeout_ms)
{
    i2c_ctx_t *c = ctx_of(bus);
    if ((c == NULL) || (data == NULL) || ((mem_size != 1u) && (mem_size != 2u))) {
        return APP_EINVAL;
    }
    HAL_StatusTypeDef st = HAL_I2C_Mem_Read(c->h, (uint16_t)(addr7 << 1), mem,
                                            (mem_size == 1u) ? I2C_MEMADD_SIZE_8BIT
                                                             : I2C_MEMADD_SIZE_16BIT,
                                            data, len, timeout_ms);
    return finish(c, bus, st);
}

int bsp_i2c_probe(bsp_i2c_bus_t bus, uint8_t addr7, uint32_t timeout_ms)
{
    i2c_ctx_t *c = ctx_of(bus);
    if (c == NULL) {
        return APP_EINVAL;
    }
    HAL_StatusTypeDef st = HAL_I2C_IsDeviceReady(c->h, (uint16_t)(addr7 << 1),
                                                 1u, timeout_ms);
    if (st == HAL_BUSY) {
        /* HAL_BUSY ≠ 裝置未回應，而是匯流排卡死（SDA 被拉住）。
         * F103 上電殘留/熱插拔的典型症狀 —— 立刻復原並重試一次，
         * 否則開機第一次探測失敗會讓上層誤判裝置不存在。 */
        (void)bsp_i2c_recover(bus);
        st = HAL_I2C_IsDeviceReady(c->h, (uint16_t)(addr7 << 1),
                                   1u, timeout_ms);
    }
    /* NAK 是常態（ack polling / 位址掃描），不列入連錯統計 */
    return (st == HAL_OK) ? APP_OK : APP_ENODEV;
}

int bsp_i2c_write_stream_dma(bsp_i2c_bus_t bus, uint8_t addr7, uint8_t prefix,
                             const uint8_t *data, uint16_t len)
{
    i2c_ctx_t *c = ctx_of(bus);
    if ((c == NULL) || (data == NULL) || (len == 0u)) {
        return APP_EINVAL;
    }
    if (c->dma_busy) {
        return APP_EBUSY;
    }
    c->dma_busy = true;
    HAL_StatusTypeDef st = HAL_I2C_Mem_Write_DMA(c->h, (uint16_t)(addr7 << 1),
                                                 prefix, I2C_MEMADD_SIZE_8BIT,
                                                 (uint8_t *)data, len);
    if (st != HAL_OK) {
        c->dma_busy = false;
        return finish(c, bus, st);
    }
    return APP_OK;
}

bool bsp_i2c_dma_busy(bsp_i2c_bus_t bus)
{
    i2c_ctx_t *c = ctx_of(bus);
    return (c != NULL) ? c->dma_busy : false;
}

const bsp_i2c_stats_t *bsp_i2c_stats(bsp_i2c_bus_t bus)
{
    i2c_ctx_t *c = ctx_of(bus);
    return (c != NULL) ? &c->stats : NULL;
}

/* ------------------------------------------------------------------ */
/*                          Bus recovery                              */
/* ------------------------------------------------------------------ */

static void od_write(GPIO_TypeDef *port, uint16_t pin, bool high)
{
    HAL_GPIO_WritePin(port, pin, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/**
 * 匯流排復原流程（I2C 規格附錄建議作法）：
 *  1. 收回 HAL 對腳位的控制，改為 GPIO 開汲極。
 *  2. 若 SDA 被從機拉低：以 ~10kHz 打最多 9 個 SCL clock，
 *     讓從機把卡住的位元移完釋放 SDA。
 *  3. 製造一個 STOP（SCL 高時 SDA 低→高），結束殘留交易。
 *  4. 重新初始化 I2C 周邊。
 */
int bsp_i2c_recover(bsp_i2c_bus_t bus)
{
    i2c_ctx_t *c = ctx_of(bus);
    if (c == NULL) {
        return APP_EINVAL;
    }
    c->stats.recover_cnt++;
    c->dma_busy = false;

    HAL_I2C_DeInit(c->h);

    GPIO_InitTypeDef io = {0};
    io.Mode  = GPIO_MODE_OUTPUT_OD;
    io.Pull  = GPIO_NOPULL;          /* 模組板載上拉負責拉高 */
    io.Speed = GPIO_SPEED_FREQ_LOW;

    io.Pin = c->scl_pin;
    od_write(c->scl_port, c->scl_pin, true);
    HAL_GPIO_Init(c->scl_port, &io);

    io.Pin = c->sda_pin;
    od_write(c->sda_port, c->sda_pin, true);
    HAL_GPIO_Init(c->sda_port, &io);

    /* 步驟 2：最多 9 個 clock 直到 SDA 釋放 */
    for (int i = 0; i < 9; i++) {
        if (HAL_GPIO_ReadPin(c->sda_port, c->sda_pin) == GPIO_PIN_SET) {
            break;
        }
        od_write(c->scl_port, c->scl_pin, false);
        bsp_delay_us(50);
        od_write(c->scl_port, c->scl_pin, true);
        bsp_delay_us(50);
    }

    /* 步驟 3：STOP condition */
    od_write(c->sda_port, c->sda_pin, false);
    bsp_delay_us(50);
    od_write(c->scl_port, c->scl_pin, true);
    bsp_delay_us(50);
    od_write(c->sda_port, c->sda_pin, true);
    bsp_delay_us(50);

    /* 步驟 4：交還 HAL（MspInit 會重設 AF 腳位） */
    HAL_StatusTypeDef st = HAL_I2C_Init(c->h);

    bool released = (HAL_GPIO_ReadPin(c->sda_port, c->sda_pin) == GPIO_PIN_SET);
    return ((st == HAL_OK) && released) ? APP_OK : APP_ERR;
}

/* ------------------------------------------------------------------ */
/*                     HAL 回呼（ISR context）                        */
/* ------------------------------------------------------------------ */

static i2c_ctx_t *ctx_by_instance(I2C_HandleTypeDef *hi2c)
{
    for (int i = 0; i < (int)BSP_I2C_BUS_COUNT; i++) {
        i2c_ctx_t *c = ctx_of((bsp_i2c_bus_t)i);
        if ((c != NULL) && (c->h != NULL) && (c->h->Instance == hi2c->Instance)) {
            return c;
        }
    }
    return NULL;
}

void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    i2c_ctx_t *c = ctx_by_instance(hi2c);
    if (c != NULL) {
        c->dma_busy = false;
        c->stats.xfer_ok++;
    }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    i2c_ctx_t *c = ctx_by_instance(hi2c);
    if (c != NULL) {
        c->dma_busy = false;
        c->stats.xfer_err++;
    }
}
