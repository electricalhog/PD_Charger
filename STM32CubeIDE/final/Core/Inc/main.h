/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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

void HAL_HRTIM_MspPostInit(HRTIM_HandleTypeDef *hhrtim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define RCC_OSC32_IN_Pin GPIO_PIN_14
#define RCC_OSC32_IN_GPIO_Port GPIOC
#define RCC_OSC32_OUT_Pin GPIO_PIN_15
#define RCC_OSC32_OUT_GPIO_Port GPIOC
#define RCC_OSC_IN_Pin GPIO_PIN_0
#define RCC_OSC_IN_GPIO_Port GPIOF
#define RCC_OSC_OUT_Pin GPIO_PIN_1
#define RCC_OSC_OUT_GPIO_Port GPIOF
#define ID_MON_Pin GPIO_PIN_1
#define ID_MON_GPIO_Port GPIOC
#define VS_MON_Pin GPIO_PIN_0
#define VS_MON_GPIO_Port GPIOA
#define IL_MON_Pin GPIO_PIN_1
#define IL_MON_GPIO_Port GPIOA
#define VS_MONA4_Pin GPIO_PIN_4
#define VS_MONA4_GPIO_Port GPIOA
#define IS_MON_Pin GPIO_PIN_0
#define IS_MON_GPIO_Port GPIOB
#define INPUT_EN_Pin GPIO_PIN_7
#define INPUT_EN_GPIO_Port GPIOC
#define OUTPUT_EN_Pin GPIO_PIN_8
#define OUTPUT_EN_GPIO_Port GPIOC
#define OUTPUT_DIS_Pin GPIO_PIN_9
#define OUTPUT_DIS_GPIO_Port GPIOC
#define PHASE_1_P_Pin GPIO_PIN_8
#define PHASE_1_P_GPIO_Port GPIOA
#define PHASE_1_N_Pin GPIO_PIN_9
#define PHASE_1_N_GPIO_Port GPIOA
#define PHASE_2_P_Pin GPIO_PIN_10
#define PHASE_2_P_GPIO_Port GPIOA
#define PHASE_2_N_Pin GPIO_PIN_11
#define PHASE_2_N_GPIO_Port GPIOA
#define VS_GOOD_Pin GPIO_PIN_12
#define VS_GOOD_GPIO_Port GPIOA
#define T_SWDIO_Pin GPIO_PIN_13
#define T_SWDIO_GPIO_Port GPIOA
#define T_SWCLK_Pin GPIO_PIN_14
#define T_SWCLK_GPIO_Port GPIOA
#define IS_GOOD_Pin GPIO_PIN_15
#define IS_GOOD_GPIO_Port GPIOA
#define T_SWO_Pin GPIO_PIN_3
#define T_SWO_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
