/**
 * @file    empl_port.h
 * @brief   InvenSense eMPL（motion driver 5.x/6.x）STM32 移植層。
 *
 * 使用方式（完整步驟見 Docs/dmp_porting.md）：
 *  1. 將官方 inv_mpu.c / inv_mpu.h / inv_mpu_dmp_motion_driver.c / .h /
 *     dmpKey.h / dmpmap.h 放到 Drivers/eMPL/。
 *  2. 在 inv_mpu.c 與 inv_mpu_dmp_motion_driver.c 開頭的
 *     「平台相依區塊」（#if defined MOTION_DRIVER_TARGET_MSP430 ... #endif）
 *     整段替換為一行：
 *         #include "drivers/empl_port.h"
 *  3. app_config.h 設定 APP_USE_MPU_DMP = 1，重新建置。
 *
 * eMPL 需要平台提供以下符號，本檔逐一對應到專案服務：
 *   i2c_write / i2c_read / delay_ms / get_ms / log_i / log_e / min()
 */
#ifndef DRV_EMPL_PORT_H
#define DRV_EMPL_PORT_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* eMPL 以此巨集選擇 MPU 型號的暫存器表 */
#ifndef MPU6050
#define MPU6050
#endif

/* inv_mpu.h 的 struct int_param_s 成員依 target 巨集而定；
 * 定義 STM32F4 target 讓它帶有 cb 欄位（僅影響結構形狀，
 * 平台相依程式碼區塊已由本檔取代，不會引用 F4 專屬標頭）。
 * ⚠ 因此本檔必須在 inv_mpu.h「之前」被 include。 */
#ifndef EMPL_TARGET_STM32F4
#define EMPL_TARGET_STM32F4
#endif

/* ---- 平台服務實作（empl_port.c） ---- */
int  empl_i2c_write(unsigned char slave_addr, unsigned char reg_addr,
                    unsigned char length, unsigned char const *data);
int  empl_i2c_read(unsigned char slave_addr, unsigned char reg_addr,
                   unsigned char length, unsigned char *data);
void empl_delay_ms(unsigned long num_ms);
void empl_get_ms(unsigned long *count);
void empl_log(const char *fmt, ...);

/* ---- eMPL 期望的符號名稱 ---- */
#define i2c_write   empl_i2c_write
#define i2c_read    empl_i2c_read
#define delay_ms    empl_delay_ms
#define get_ms      empl_get_ms
#define log_i       empl_log
#define log_e       empl_log

#ifndef min
#define min(a, b)   ((a) < (b) ? (a) : (b))
#endif

#define __no_operation()  __asm volatile ("nop")

#ifdef __cplusplus
}
#endif

#endif /* DRV_EMPL_PORT_H */
