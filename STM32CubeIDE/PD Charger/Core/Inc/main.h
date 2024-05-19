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
#include "stm32g4xx_ll_ucpd.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_cortex.h"
#include "stm32g4xx_ll_rcc.h"
#include "stm32g4xx_ll_system.h"
#include "stm32g4xx_ll_utils.h"
#include "stm32g4xx_ll_pwr.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_dma.h"

#include "stm32g4xx_ll_exti.h"

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
#define Reg_EN_Pin GPIO_PIN_13
#define Reg_EN_GPIO_Port GPIOC
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
#define UVLO_Pin GPIO_PIN_5
#define UVLO_GPIO_Port GPIOA
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
#define I2C2_INT_Pin GPIO_PIN_10
#define I2C2_INT_GPIO_Port GPIOA
#define Discharge_Pin GPIO_PIN_10
#define Discharge_GPIO_Port GPIOC
#define Enable_Pin GPIO_PIN_11
#define Enable_GPIO_Port GPIOC

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
