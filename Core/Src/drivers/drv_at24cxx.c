/**
 * @file    drv_at24cxx.c
 * @brief   AT24Cxx EEPROM 驅動實作。
 */
#include "drivers/drv_at24cxx.h"
#include "bsp/bsp_i2c.h"
#include "bsp/bsp_board.h"
#include "bsp/bsp_uart.h"
#include "app/app_config.h"
#include <string.h>

#define AT24_XFER_TIMEOUT_MS   20u
#define AT24_WRITE_POLL_MS     15u   /* 資料手冊 tWR 最大 5~10ms，留裕度 */

typedef struct {
    uint16_t total_size;
    uint8_t  page_size;
    uint8_t  addr_bytes;      /* 記憶體位址長度 1/2 */
    uint8_t  blk_addr_mask;   /* 借用裝置位址的高位址位元遮罩 */
} at24_geom_t;

#if   (APP_AT24_TYPE == AT24C02)
static const at24_geom_t s_geom_cfg = { 256u,   8u, 1u, 0x00u };
#elif (APP_AT24_TYPE == AT24C04)
static const at24_geom_t s_geom_cfg = { 512u,  16u, 1u, 0x01u };
#elif (APP_AT24_TYPE == AT24C08)
static const at24_geom_t s_geom_cfg = { 1024u, 16u, 1u, 0x03u };
#elif (APP_AT24_TYPE == AT24C16)
static const at24_geom_t s_geom_cfg = { 2048u, 16u, 1u, 0x07u };
#elif (APP_AT24_TYPE == AT24C32)
static const at24_geom_t s_geom_cfg = { 4096u, 32u, 2u, 0x00u };
#elif (APP_AT24_TYPE == AT24C64)
static const at24_geom_t s_geom_cfg = { 8192u, 32u, 2u, 0x00u };
#else
#error "APP_AT24_TYPE 未支援，請於 app_config.h 選擇正確型號"
#endif

/* 幾何自動修正的備選：市售套件最常見的「型號誤設」是 C02 ↔ C32
 * （2 種定址長度）。開機讀寫自檢失敗時切換另一種再驗證。 */
static const at24_geom_t s_geom_c02 = { 256u,  8u,  1u, 0x00u };
static const at24_geom_t s_geom_c32 = { 4096u, 32u, 2u, 0x00u };

static at24_geom_t s_geom;          /* 執行期幾何（可能被自檢修正） */
static bool s_geom_ready;

static void ensure_geom(void)
{
    if (!s_geom_ready) {
        s_geom = s_geom_cfg;
        s_geom_ready = true;
    }
}

static uint8_t s_dev_base;   /* at24_probe() 掃描到的位址；0 = 未找到 */

/* 依記憶體位址算出實際 7-bit 裝置位址（C04/08/16 借位） */
static uint8_t dev_addr_of(uint16_t mem)
{
    uint8_t dev = (s_dev_base != 0u) ? s_dev_base : BSP_ADDR_AT24_BASE;
    if ((s_geom.addr_bytes == 1u) && (s_geom.blk_addr_mask != 0u)) {
        dev |= (uint8_t)((mem >> 8) & s_geom.blk_addr_mask);
    }
    return dev;
}

/* 本筆傳輸在「同一裝置位址視窗」內最多可到的長度：
 * 1-byte 定址時，每 256B 為一個視窗（跨視窗需換裝置位址） */
static uint16_t window_remain(uint16_t mem)
{
    if (s_geom.addr_bytes == 1u) {
        return (uint16_t)(256u - (mem & 0xFFu));
    }
    return (uint16_t)(s_geom.total_size - mem);
}

/**
 * @brief 讀寫自檢：在儲存布局外的抓痕區寫入隨機標記並讀回。
 *
 * 抓痕位址 = 幾何尾端 8B。兩種幾何下都不會碰到資料區
 * （槽 0x00~0x3F、映射 0x40~0xDF）；C32 被誤當 C02 寫入時，
 * 1-byte 位址落在其高位區（≥0x800），同樣不傷資料。
 */
static int rw_selftest(void)
{
    uint16_t addr = (uint16_t)(s_geom.total_size - 8u);
    uint8_t pat[4];
    uint8_t back[4] = {0};
    uint32_t t = HAL_GetTick();
    pat[0] = 0xA5u;
    pat[1] = (uint8_t)t;            /* 每次開機不同，避免「上次殘值」誤判 */
    pat[2] = 0x5Au;
    pat[3] = (uint8_t)~t;

    if (at24_write(addr, pat, sizeof(pat)) != APP_OK) {
        return APP_ERR;
    }
    if (at24_read(addr, back, sizeof(back)) != APP_OK) {
        return APP_ERR;
    }
    return (memcmp(pat, back, sizeof(pat)) == 0) ? APP_OK : APP_ECRC;
}

int at24_probe(void)
{
    ensure_geom();

    /* 位址自動掃描：0x50 起，跨距避開 C04/08/16 借用的區塊位元
     * （C02 逐一掃 0x50~0x57；C08 只可能在 0x50/0x54；C16 只有 0x50） */
    uint8_t step = (s_geom.addr_bytes == 1u)
                   ? (uint8_t)(s_geom.blk_addr_mask + 1u) : 1u;
    s_dev_base = 0u;
    for (uint16_t base = 0x50u; base <= 0x57u; base += step) {
        if (bsp_i2c_probe(BSP_I2C_SENSOR, (uint8_t)base,
                          AT24_XFER_TIMEOUT_MS) == APP_OK) {
            s_dev_base = (uint8_t)base;
            break;
        }
    }
    if (s_dev_base == 0u) {
        return APP_ENODEV;
    }

    /* 定址幾何自檢與自動修正：型號誤設（C02↔C32）時寫入會位移、
     * 驗證必敗 —— 這裡當場換另一種定址再試，選通過的那種。 */
    if (rw_selftest() == APP_OK) {
        return APP_OK;
    }
    at24_geom_t saved = s_geom;
    s_geom = (saved.addr_bytes == 1u) ? s_geom_c32 : s_geom_c02;
    if (rw_selftest() == APP_OK) {
        dbg_printf("[at24] geometry auto-corrected -> %u-byte addr, %uB\r\n",
                   (unsigned)s_geom.addr_bytes, (unsigned)s_geom.total_size);
        return APP_OK;
    }
    s_geom = saved;
    dbg_printf("[at24] R/W selftest FAILED (WP pin high? unsupported chip?)\r\n");
    return APP_OK;   /* 在線但寫入不可靠：commits 會失敗且 W/F 可觀測 */
}

uint8_t at24_dev_addr(void)
{
    return s_dev_base;
}

uint8_t at24_addr_bytes(void)
{
    ensure_geom();
    return s_geom.addr_bytes;
}

uint16_t at24_size(void)
{
    ensure_geom();
    return s_geom.total_size;
}

int at24_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    ensure_geom();
    if ((buf == NULL) || ((uint32_t)addr + len > s_geom.total_size)) {
        return APP_EINVAL;
    }
    while (len > 0u) {
        uint16_t chunk = APP_MIN(len, window_remain(addr));
        uint16_t mem = (s_geom.addr_bytes == 1u) ? (addr & 0xFFu) : addr;
        int rc = bsp_i2c_mem_read(BSP_I2C_SENSOR, dev_addr_of(addr), mem,
                                  s_geom.addr_bytes, buf, chunk,
                                  AT24_XFER_TIMEOUT_MS);
        if (rc != APP_OK) {
            return rc;
        }
        addr += chunk;
        buf  += chunk;
        len  -= chunk;
    }
    return APP_OK;
}

/* ACK polling：寫入週期中裝置不回 ACK，輪詢直到回應或逾時 */
static int wait_write_done(uint8_t dev)
{
    uint32_t start = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - start) < AT24_WRITE_POLL_MS) {
        if (bsp_i2c_probe(BSP_I2C_SENSOR, dev, 2u) == APP_OK) {
            return APP_OK;
        }
    }
    return APP_ETIMEOUT;
}

int at24_write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    ensure_geom();
    if ((buf == NULL) || ((uint32_t)addr + len > s_geom.total_size)) {
        return APP_EINVAL;
    }
    while (len > 0u) {
        /* 拆筆：不可跨頁，也不可跨 256B 定址視窗 */
        uint16_t page_remain = (uint16_t)(s_geom.page_size
                                          - (addr % s_geom.page_size));
        uint16_t chunk = APP_MIN(len, page_remain);
        chunk = APP_MIN(chunk, window_remain(addr));

        uint8_t dev = dev_addr_of(addr);
        uint16_t mem = (s_geom.addr_bytes == 1u) ? (addr & 0xFFu) : addr;
        int rc = bsp_i2c_mem_write(BSP_I2C_SENSOR, dev, mem,
                                   s_geom.addr_bytes, buf, chunk,
                                   AT24_XFER_TIMEOUT_MS);
        if (rc != APP_OK) {
            return rc;
        }
        rc = wait_write_done(dev);
        if (rc != APP_OK) {
            return rc;
        }
        addr += chunk;
        buf  += chunk;
        len  -= chunk;
    }
    return APP_OK;
}
