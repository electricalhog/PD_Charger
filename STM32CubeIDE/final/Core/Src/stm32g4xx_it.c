/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32g4xx_it.c
  * @brief   Interrupt Service Routines.
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

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32g4xx_it.h"
#include "usbpd.h"
#include "tracer_emb.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "regulator.h"
#include "slope_comp.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern DMA_HandleTypeDef hdma_adc1;
extern DMA_HandleTypeDef hdma_adc2;
extern DMA_HandleTypeDef hdma_lpuart1_tx;
extern UART_HandleTypeDef hlpuart1;
extern TIM_HandleTypeDef htim2;

/* USER CODE BEGIN EV */
/* Peripheral handles needed by regulator ISRs */
extern TIM_HandleTypeDef    htim6;
extern TIM_HandleTypeDef    htim7;
extern HRTIM_HandleTypeDef  hhrtim1;
/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32G4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32g4xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles DMA1 channel2 global interrupt.
  */
void DMA1_Channel2_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel2_IRQn 0 */

  /* USER CODE END DMA1_Channel2_IRQn 0 */
  /* USER CODE BEGIN DMA1_Channel2_IRQn 1 */

  /* USER CODE END DMA1_Channel2_IRQn 1 */
}

/**
  * @brief This function handles DMA1 channel3 global interrupt.
  */
void DMA1_Channel3_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel3_IRQn 0 */

  /* USER CODE END DMA1_Channel3_IRQn 0 */
  TRACER_EMB_IRQHandlerDMA();
  /* USER CODE BEGIN DMA1_Channel3_IRQn 1 */

  /* USER CODE END DMA1_Channel3_IRQn 1 */
}

/**
  * @brief This function handles DMA1 channel4 global interrupt.
  */
void DMA1_Channel4_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel4_IRQn 0 */

  /* USER CODE END DMA1_Channel4_IRQn 0 */
  /* USER CODE BEGIN DMA1_Channel4_IRQn 1 */

  /* USER CODE END DMA1_Channel4_IRQn 1 */
}

/**
  * @brief This function handles DMA1 channel5 global interrupt.
  */
void DMA1_Channel5_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel5_IRQn 0 */

  /* USER CODE END DMA1_Channel5_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_adc1);
  /* USER CODE BEGIN DMA1_Channel5_IRQn 1 */

  /* USER CODE END DMA1_Channel5_IRQn 1 */
}

/**
  * @brief This function handles DMA1 channel6 global interrupt.
  */
void DMA1_Channel6_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel6_IRQn 0 */

  /* USER CODE END DMA1_Channel6_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_adc2);
  /* USER CODE BEGIN DMA1_Channel6_IRQn 1 */

  /* USER CODE END DMA1_Channel6_IRQn 1 */
}

/**
  * @brief This function handles TIM2 global interrupt.
  */
void TIM2_IRQHandler(void)
{
  /* USER CODE BEGIN TIM2_IRQn 0 */

  /* USER CODE END TIM2_IRQn 0 */
  HAL_TIM_IRQHandler(&htim2);
  /* USER CODE BEGIN TIM2_IRQn 1 */

  /* USER CODE END TIM2_IRQn 1 */
}

/**
  * @brief This function handles UCPD1 interrupt / UCPD1 wake-up interrupt through EXTI line 43.
  */
void UCPD1_IRQHandler(void)
{
  /* USER CODE BEGIN UCPD1_IRQn 0 */

  /* USER CODE END UCPD1_IRQn 0 */
  USBPD_PORT0_IRQHandler();

  /* USER CODE BEGIN UCPD1_IRQn 1 */

  /* USER CODE END UCPD1_IRQn 1 */
}

/**
  * @brief This function handles LPUART1 global interrupt.
  */
void LPUART1_IRQHandler(void)
{
  /* USER CODE BEGIN LPUART1_IRQn 0 */

  /* USER CODE END LPUART1_IRQn 0 */
  TRACER_EMB_IRQHandlerUSART();
  /* USER CODE BEGIN LPUART1_IRQn 1 */

  /* USER CODE END LPUART1_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/**
 * @brief TIM6 and DAC underrun interrupt handler.
 *        TIM6 is used for slope compensation (2 MHz, priority 0).
 *        Called every 500 ns while the regulator is RUNNING.
 *        ISR body is in slope_comp.c (§7.5, §13.2).
 */
void TIM6_DAC_IRQHandler(void)
{
  /* Clear the TIM6 update interrupt flag */
  if (__HAL_TIM_GET_FLAG(&htim6, TIM_FLAG_UPDATE) &&
      __HAL_TIM_GET_IT_SOURCE(&htim6, TIM_IT_UPDATE))
  {
    __HAL_TIM_CLEAR_IT(&htim6, TIM_IT_UPDATE);
    slope_comp_tim6_isr();
  }
}

/**
 * @brief TIM7/DAC2/DAC4 shared interrupt handler.
 *        On STM32G474, TIM7 shares IRQ 55 (TIM7_DAC_IRQn) with DAC2/DAC4
 *        underrun events.  The vector must be named TIM7_DAC_IRQHandler.
 *        TIM7 is used for the PID outer control loop (20 kHz, priority 2).
 *        ISR body is in regulator.c (§8, §13.2).
 */
void TIM7_DAC_IRQHandler(void)
{
  /* Clear the TIM7 update interrupt flag */
  if (__HAL_TIM_GET_FLAG(&htim7, TIM_FLAG_UPDATE) &&
      __HAL_TIM_GET_IT_SOURCE(&htim7, TIM_IT_UPDATE))
  {
    __HAL_TIM_CLEAR_IT(&htim7, TIM_IT_UPDATE);
    regulator_pid_tim7_isr();
  }
}

/**
 * @brief HRTIM1 Timer A interrupt handler.
 *        Fires on every switching period reset (200 kHz, priority 1).
 *        Reloads DAC3 CH1 with the current PID peak value (§7.6).
 */
void HRTIM1_TIMA_IRQHandler(void)
{
  /* Check for repetition (period) interrupt */
  if (__HAL_HRTIM_TIMER_GET_ITSTATUS(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                      HRTIM_TIM_IT_REP))
  {
    __HAL_HRTIM_TIMER_CLEAR_IT(&hhrtim1, HRTIM_TIMERINDEX_TIMER_A,
                                HRTIM_TIM_IT_REP);
    regulator_hrtim_tima_period_isr();
  }
}

/**
 * @brief HRTIM1 fault interrupt handler.
 *        Fires when FLT1 (VS_GOOD, PA12) or FLT2 (IS_GOOD, PA15) asserts
 *        (active-low).  Priority 1.  ISR body is in regulator.c (§10.1).
 */
void HRTIM1_FLT_IRQHandler(void)
{
  regulator_hrtim_fault_isr();
  /* Flag clearing is performed inside regulator_hrtim_fault_isr() */
}

/* USER CODE END 1 */
