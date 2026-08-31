/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * IMU-BLE Node ??ôÂ≠∏Â∞àÊ?àÔ?àSTM32F103C8T6Ôº?
  * ?ú¨Ê™îÁî± CubeMX ?î¢???/Á∂≠Ë≠∑ÔºåÊ?âÁî®Á®ãÂ?èÁ¢º‰∏?ÂæãÊîæ?ú® USER CODE ??Â°äÂÖß??
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LED_STATUS_Pin GPIO_PIN_13
#define LED_STATUS_GPIO_Port GPIOC
#define KEY0_Pin GPIO_PIN_0
#define KEY0_GPIO_Port GPIOA
#define KEY1_Pin GPIO_PIN_1
#define KEY1_GPIO_Port GPIOA
#define MPU_INT_Pin GPIO_PIN_5
#define MPU_INT_GPIO_Port GPIOB
#define MPU_INT_EXTI_IRQn EXTI9_5_IRQn

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
