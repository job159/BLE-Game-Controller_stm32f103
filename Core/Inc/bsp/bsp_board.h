/**
 * @file    bsp_board.h
 * @brief   板級對應表（Board Support Package - 硬體映射唯一入口）。
 *
 * ============================ 可移植性核心 ============================
 * 上層（drivers / mw / app）「絕不」直接出現 GPIOx / hi2cx / huartx，
 * 一律透過本檔的巨集與 bsp_*.h 的介面存取硬體。
 * 換板、改腳位、換 MCU 時：改這個檔 + CubeMX 設定即可。
 * =====================================================================
 *
 * 參考接線（STM32F103C8T6, LQFP48）：
 *   I2C1  PB6=SCL / PB7=SDA   : MPU6050(0x68) + AT24Cxx(0x50)（感測/儲存匯流排）
 *   I2C2  PB10=SCL / PB11=SDA : SSD1306 OLED(0x3C)（顯示匯流排，走 DMA）
 *   USART1 PA9=TX / PA10=RX   : 除錯主控台 / CLI（115200 8N1）
 *   USART2 PA2=TX / PA3=RX    : nRF52832 BLE 透傳模組（115200 8N1）
 *   PA0 = KEY6（低電位按下，內部上拉；本地功能：點擊計數/空中滑鼠雙擊/長按歸零）
 *   PA1 = KEY1（同上；UI 操作：換頁/遙測/校正）
 *   PA4~PA7 = KEY2~KEY5（手柄鍵，事件經 BLE 上報由 PC 映射）
 *   PB0 = KEY0、PB1 = KEY7（手柄鍵，同 KEY2~5）
 *   PB5 = MPU6050 INT（下降緣 EXTI）
 *   PC13 = 狀態 LED（低電位點亮）
 */
#ifndef BSP_BOARD_H
#define BSP_BOARD_H

#include "main.h"          /* CubeMX 腳位定義 + HAL */
#include "app/app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CubeMX 產生之周邊 handle（定義位於 Core/Src/main.c） ---- */
extern I2C_HandleTypeDef  hi2c1;
extern I2C_HandleTypeDef  hi2c2;
extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;
extern IWDG_HandleTypeDef hiwdg;
extern DMA_HandleTypeDef  hdma_usart2_rx;
extern DMA_HandleTypeDef  hdma_usart2_tx;
extern DMA_HandleTypeDef  hdma_i2c2_tx;

/* ---- 角色 → 實體周邊對應 ---- */
#define BSP_UART_DBG_HANDLE     (&huart1)   /* 除錯 / CLI  */
#define BSP_UART_BLE_HANDLE     (&huart2)   /* BLE 透傳    */

#define BSP_I2C_SENSOR_HANDLE   (&hi2c1)    /* MPU6050 + AT24Cxx */
#define BSP_I2C_DISP_HANDLE     (&hi2c2)    /* OLED（DMA）        */

/* I2C 匯流排復原（bus recovery）需要直接操作腳位 */
#define BSP_I2C1_SCL_PORT       GPIOB
#define BSP_I2C1_SCL_PIN        GPIO_PIN_6
#define BSP_I2C1_SDA_PORT       GPIOB
#define BSP_I2C1_SDA_PIN        GPIO_PIN_7
#define BSP_I2C2_SCL_PORT       GPIOB
#define BSP_I2C2_SCL_PIN        GPIO_PIN_10
#define BSP_I2C2_SDA_PORT       GPIOB
#define BSP_I2C2_SDA_PIN        GPIO_PIN_11

/* ---- I2C 裝置位址（7-bit） ---- */
#define BSP_ADDR_MPU6050        0x68u   /* AD0=0 */
#define BSP_ADDR_AT24_BASE      0x50u   /* A2..A0 = 0 */
#define BSP_ADDR_SSD1306        0x3Cu   /* 部分模組為 0x3D，驅動會自動探測 */

/* ---- LED（PC13 低電位點亮） ---- */
#define BSP_LED_ON()      HAL_GPIO_WritePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin, GPIO_PIN_RESET)
#define BSP_LED_OFF()     HAL_GPIO_WritePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin, GPIO_PIN_SET)
#define BSP_LED_TOGGLE()  HAL_GPIO_TogglePin(LED_STATUS_GPIO_Port, LED_STATUS_Pin)

/* ---- 按鍵原始讀取（true = 實體按下；低電位有效） ---- */
#define BSP_KEY0_PRESSED()  (HAL_GPIO_ReadPin(KEY0_GPIO_Port, KEY0_Pin) == GPIO_PIN_RESET)
#define BSP_KEY1_PRESSED()  (HAL_GPIO_ReadPin(KEY1_GPIO_Port, KEY1_Pin) == GPIO_PIN_RESET)
#define BSP_KEY2_PRESSED()  (HAL_GPIO_ReadPin(KEY2_GPIO_Port, KEY2_Pin) == GPIO_PIN_RESET)
#define BSP_KEY3_PRESSED()  (HAL_GPIO_ReadPin(KEY3_GPIO_Port, KEY3_Pin) == GPIO_PIN_RESET)
#define BSP_KEY4_PRESSED()  (HAL_GPIO_ReadPin(KEY4_GPIO_Port, KEY4_Pin) == GPIO_PIN_RESET)
#define BSP_KEY5_PRESSED()  (HAL_GPIO_ReadPin(KEY5_GPIO_Port, KEY5_Pin) == GPIO_PIN_RESET)
#define BSP_KEY6_PRESSED()  (HAL_GPIO_ReadPin(KEY6_GPIO_Port, KEY6_Pin) == GPIO_PIN_RESET)
#define BSP_KEY7_PRESSED()  (HAL_GPIO_ReadPin(KEY7_GPIO_Port, KEY7_Pin) == GPIO_PIN_RESET)

#ifdef __cplusplus
}
#endif

#endif /* BSP_BOARD_H */
