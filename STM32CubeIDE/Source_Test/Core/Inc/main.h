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
#include "stm32g4xx_nucleo.h"

#include "stm32g4xx_ll_lpuart.h"
#include "stm32g4xx_ll_rcc.h"
#include "stm32g4xx_ll_ucpd.h"
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_cortex.h"
#include "stm32g4xx_ll_system.h"
#include "stm32g4xx_ll_utils.h"
#include "stm32g4xx_ll_pwr.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_dma.h"

#include "stm32g4xx_ll_exti.h"

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

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define Input_en_Pin GPIO_PIN_6
#define Input_en_GPIO_Port GPIOC
#define Output_en_Pin GPIO_PIN_7
#define Output_en_GPIO_Port GPIOC
#define Output_dischg_Pin GPIO_PIN_8
#define Output_dischg_GPIO_Port GPIOC
#define VSgood_Pin GPIO_PIN_9
#define VSgood_GPIO_Port GPIOC
#define FiveVgood_Pin GPIO_PIN_8
#define FiveVgood_GPIO_Port GPIOA
#define ThreeVgood_Pin GPIO_PIN_9
#define ThreeVgood_GPIO_Port GPIOA
#define PH_2_P_Pin GPIO_PIN_14
#define PH_2_P_GPIO_Port GPIOA
#define PH_1_P_Pin GPIO_PIN_15
#define PH_1_P_GPIO_Port GPIOA
#define PH_1_N_Pin GPIO_PIN_10
#define PH_1_N_GPIO_Port GPIOC
#define PH_2_N_Pin GPIO_PIN_11
#define PH_2_N_GPIO_Port GPIOC
#define ISgood_Pin GPIO_PIN_2
#define ISgood_GPIO_Port GPIOD

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
