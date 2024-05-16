/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
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
#include "stm32g4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
// #include "FUSB302.h"

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
#define Vinsense_Pin GPIO_PIN_0
#define Vinsense_GPIO_Port GPIOA
#define Voutsense_Pin GPIO_PIN_1
#define Voutsense_GPIO_Port GPIOA
#define Vctrlv_Pin GPIO_PIN_2
#define Vctrlv_GPIO_Port GPIOA
#define Vadj_Pin GPIO_PIN_3
#define Vadj_GPIO_Port GPIOA
#define Vctrli_Pin GPIO_PIN_4
#define Vctrli_GPIO_Port GPIOA
#define Run_Pin GPIO_PIN_5
#define Run_GPIO_Port GPIOA
#define VadjA6_Pin GPIO_PIN_6
#define VadjA6_GPIO_Port GPIOA
#define VadjA7_Pin GPIO_PIN_7
#define VadjA7_GPIO_Port GPIOA
#define SYNC_Pin GPIO_PIN_0
#define SYNC_GPIO_Port GPIOB
#define ISmon_Pin GPIO_PIN_1
#define ISmon_GPIO_Port GPIOB
#define ISmonB2_Pin GPIO_PIN_2
#define ISmonB2_GPIO_Port GPIOB
#define Discharge_Pin GPIO_PIN_10
#define Discharge_GPIO_Port GPIOC
#define Enable_Pin GPIO_PIN_11
#define Enable_GPIO_Port GPIOC
#define I2C1_SDA_Pin GPIO_PIN_7
#define I2C1_SDA_GPIO_Port GPIOB
#define I2C1_SCL_Pin GPIO_PIN_8
#define I2C1_SCL_GPIO_Port GPIOB
#define I2C1_INT_Pin GPIO_PIN_9
#define I2C1_INT_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
