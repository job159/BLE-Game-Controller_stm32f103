/**
 * @file    empl_port.c
 * @brief   eMPL 平台服務實作（僅在 APP_USE_MPU_DMP == 1 時編譯）。
 */
#include "app/app_config.h"
#if (APP_USE_MPU_DMP == 1)

#include "drivers/empl_port.h"
#include "bsp/bsp_i2c.h"
#include "bsp/bsp_uart.h"
#include "stm32f1xx_hal.h"
#include <stdarg.h>
#include <stdio.h>

/* eMPL 契約：回傳 0 = 成功，非 0 = 失敗 */

int empl_i2c_write(unsigned char slave_addr, unsigned char reg_addr,
                   unsigned char length, unsigned char const *data)
{
    return (bsp_i2c_mem_write(BSP_I2C_SENSOR, slave_addr, reg_addr, 1u,
                              data, length, 100u) == APP_OK) ? 0 : -1;
}

int empl_i2c_read(unsigned char slave_addr, unsigned char reg_addr,
                  unsigned char length, unsigned char *data)
{
    return (bsp_i2c_mem_read(BSP_I2C_SENSOR, slave_addr, reg_addr, 1u,
                             data, length, 100u) == APP_OK) ? 0 : -1;
}

void empl_delay_ms(unsigned long num_ms)
{
    HAL_Delay(num_ms);
}

void empl_get_ms(unsigned long *count)
{
    if (count != NULL) {
        *count = HAL_GetTick();
    }
}

void empl_log(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        dbg_printf("[eMPL] %s\r\n", buf);
    }
}

#endif /* APP_USE_MPU_DMP == 1 */
