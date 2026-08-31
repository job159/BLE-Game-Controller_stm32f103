/**
 * @file    drv_blemod.c
 * @brief   BLE 透傳模組自動佈建實作。
 *
 * AT 方言差異（汇承家族內部就不一致，實測踩過的坑）：
 *   - HC-08 / HC-42：命令「不帶結尾」，bare "AT" 即回 OK；
 *     帶了 \r\n 反而可能整包不認。
 *   - HC-05 / HM 系與部分新韌體：要求 "\r\n" 結尾。
 * 因此每個鮑率各探兩種方言（bare 優先），改寫鮑率時沿用
 * 探測成功的方言，且命令語法準備兩種變體（AT+BAUD=x 與
 * AT+BAUD=x,N）。
 *
 * 這是初始化階段的「已知長阻塞操作」（最壞 ~2 秒）：
 * 依 bsp_wdg.h 規範於每次探測前手動餵狗。
 */
#include "drivers/drv_blemod.h"
#include "bsp/bsp_uart.h"
#include "bsp/bsp_wdg.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <string.h>

/* 候選鮑率（實際探測順序：target 最先，之後依此表跳過重複） */
static const uint32_t s_bauds[] = { 115200u, 9600u, 57600u, 38400u, 19200u };

#define AT_PROBE_WAIT_MS   120u
#define AT_CMD_WAIT_MS     300u

/**
 * @brief 送出命令（可選 CRLF 結尾）並在 wait_ms 內等待回應含 "OK"。
 * @note  送出前清空 RX 殘料；回應以滑動比對搜尋，容忍任何前後綴。
 */
static bool at_ok(const char *cmd, bool crlf, uint32_t wait_ms)
{
    uint8_t junk[32];
    for (int i = 0; (i < 8) && (ble_read(junk, sizeof(junk)) > 0u); i++) {
        /* 清空舊資料（有界，避免資料湧入時卡住） */
    }

    (void)ble_write(cmd, (uint16_t)strlen(cmd));
    if (crlf) {
        (void)ble_write("\r\n", 2u);
    }

    uint32_t t0 = HAL_GetTick();
    uint8_t prev = 0u;
    while ((uint32_t)(HAL_GetTick() - t0) < wait_ms) {
        uint8_t b;
        while (ble_read(&b, 1u) == 1u) {
            if ((prev == (uint8_t)'O') && (b == (uint8_t)'K')) {
                return true;
            }
            prev = b;
        }
    }
    return false;
}

blemod_result_t blemod_autobaud(uint32_t target_baud, uint32_t *found_baud)
{
    uint32_t found = 0u;
    bool crlf = false;

    /* i == -1 代表 target（最先探，已佈建的模組最快路徑收斂） */
    for (int i = -1; i < (int)APP_ARRAY_COUNT(s_bauds); i++) {
        uint32_t baud = (i < 0) ? target_baud : s_bauds[i];
        if ((i >= 0) && (baud == target_baud)) {
            continue;   /* target 已探過 */
        }
        if (ble_set_baud(baud) != APP_OK) {
            continue;
        }
        HAL_Delay(20);        /* 鮑率切換後讓線路靜一下 */

        bsp_wdg_feed_raw();   /* 已知長操作：逐次探測餵狗 */
        if (at_ok("AT", false, AT_PROBE_WAIT_MS)) {
            found = baud;
            crlf = false;     /* HC-08/HC-42 式：無結尾 */
            break;
        }
        bsp_wdg_feed_raw();
        if (at_ok("AT", true, AT_PROBE_WAIT_MS)) {
            found = baud;
            crlf = true;      /* HC-05/HM 式：CRLF 結尾 */
            break;
        }
    }

    if (found_baud != NULL) {
        *found_baud = found;
    }

    if (found == 0u) {
        (void)ble_set_baud(target_baud);   /* 恢復工作鮑率 */
        return BLEMOD_NOT_FOUND;
    }
    if (found == target_baud) {
        return BLEMOD_OK_ALREADY;          /* 已在目標鮑率 */
    }

    /* 在別的鮑率找到模組 → 改寫（兩種命令語法輪流試） */
    char cmd[32];
    bool acked = false;
    (void)snprintf(cmd, sizeof(cmd), "AT+BAUD=%lu", (unsigned long)target_baud);
    bsp_wdg_feed_raw();
    acked = at_ok(cmd, crlf, AT_CMD_WAIT_MS);
    if (!acked) {
        (void)snprintf(cmd, sizeof(cmd), "AT+BAUD=%lu,N",
                       (unsigned long)target_baud);
        bsp_wdg_feed_raw();
        acked = at_ok(cmd, crlf, AT_CMD_WAIT_MS);
    }

    (void)ble_set_baud(target_baud);
    if (!acked) {
        return BLEMOD_NOT_FOUND;           /* 有回 AT 但不吃 BAUD 命令：
                                              當作不支援的方言 */
    }

    /* 模組切換需要一點時間；驗證失敗不視為致命（多半已生效） */
    HAL_Delay(150);
    bsp_wdg_feed_raw();
    (void)at_ok("AT", crlf, AT_PROBE_WAIT_MS);
    return BLEMOD_OK_FIXED;
}
