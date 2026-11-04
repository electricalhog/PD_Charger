/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbpd_devices_conf.h
  * @author  MCD Application Team
  * @brief   This file contains the device define.
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

/* CubeMX Generated */
#define CUBEMX_GENERATED

#ifndef USBPD_DEVICE_CONF_H
#define USBPD_DEVICE_CONF_H

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_dma.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_rcc.h"
#include "stm32g4xx_ll_ucpd.h"
#include "stm32g4xx_ll_pwr.h"
#include "stm32g4xx_ll_tim.h"
#include "usbpd_pwr_user.h"
#include "usbpd_pwr_if.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/

/* -----------------------------------------------------------------------------
      usbpd_hw.c
-------------------------------------------------------------------------------*/

/* defined used to configure function : BSP_USBPD_GetUSPDInstance */
#define UCPD_INSTANCE0 UCPD1

/* defined used to configure function : BSP_USBPD_Init_DMARxInstance,BSP_USBPD_DeInit_DMARxInstance */
#define UCPDDMA_INSTANCE0_CLOCKENABLE_RX  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1)

#define UCPDDMA_INSTANCE0_DMA_RX  DMA1

#define UCPDDMA_INSTANCE0_REQUEST_RX   DMA_REQUEST_UCPD1_RX

#define UCPDDMA_INSTANCE0_LL_CHANNEL_RX   LL_DMA_CHANNEL_4

#define UCPDDMA_INSTANCE0_CHANNEL_RX   DMA1_Channel4

/* defined used to configure function : BSP_USBPD_Init_DMATxInstance, BSP_USBPD_DeInit_DMATxInstance */
#define UCPDDMA_INSTANCE0_CLOCKENABLE_TX  LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1)

#define UCPDDMA_INSTANCE0_DMA_TX  DMA1

#define UCPDDMA_INSTANCE0_REQUEST_TX   DMA_REQUEST_UCPD1_TX

#define UCPDDMA_INSTANCE0_LL_CHANNEL_TX   LL_DMA_CHANNEL_2

#define UCPDDMA_INSTANCE0_CHANNEL_TX   DMA1_Channel2
/* defined used to configure  BSP_USBPD_SetFRSSignalling */
#define UCPDFRS_INSTANCE0_FRSCC1
#define UCPDFRS_INSTANCE0_FRSCC2

/* Priority must be >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5) because
 * USBPD_CAD_WakeUp() calls osMessagePut() -> xQueueSendFromISR() from this ISR.
 * Priority 4 (the previous value) is above the FreeRTOS syscall ceiling and
 * causes the configASSERT in vPortValidateInterruptPriority() to fire. */
#define UCPD_INSTANCE0_ENABLEIRQ   do{                                                                 \
                                        NVIC_SetPriority(UCPD1_IRQn,5);                              \
                                        NVIC_EnableIRQ(UCPD1_IRQn);                                  \
                                    } while(0)

/* -----------------------------------------------------------------------------
      Definitions for timer service feature
-------------------------------------------------------------------------------*/

#define TIMX                           TIM1
#define TIMX_CLK_ENABLE                LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_TIM1)
#define TIMX_CLK_DISABLE               LL_APB2_GRP1_DisableClock(LL_APB2_GRP1_PERIPH_TIM1)
#define TIMX_CHANNEL_CH1               LL_TIM_CHANNEL_CH1
#define TIMX_CHANNEL_CH2               LL_TIM_CHANNEL_CH2
#define TIMX_CHANNEL_CH3               LL_TIM_CHANNEL_CH3
#define TIMX_CHANNEL_CH4               LL_TIM_CHANNEL_CH4
/* Arm one timer-server channel TimeUs from now.  The compare write and the
 * flag clear are one critical section (a few cycles): ST's version did them
 * with interrupts on, so a regulator ISR (priority 0-2, up to ~45 us) between
 * them let the 30 us GoodCRC compare match during the preemption, and the
 * clear then erased that match.  The next one came a full TIM1 wrap (10 ms)
 * later, PRL_GoodCRC busy-waited for it, and the sink gave up on its
 * EPR_KeepAlive: Soft_Reset, then Hard Reset (bench 2026-11-03, 28 V EPR
 * drops every 7-90 s; ucpd_ring: RX end -> GoodCRC 10034 us vs 37-49 us). */
#define TIMX_SETEVENT(_ch_)            do{                                                                    \
                                          const uint32_t _pm = __get_PRIMASK();                               \
                                          __disable_irq();                                                    \
                                          LL_TIM_OC_SetCompareCH##_ch_(TIMX, (TimeUs + TIMX->CNT) % TIM_MAX_TIME);\
                                          LL_TIM_ClearFlag_CC##_ch_(TIMX);                                    \
                                          __set_PRIMASK(_pm);                                                 \
                                       }while(0)
#define TIMX_CHANNEL1_SETEVENT         TIMX_SETEVENT(1)
#define TIMX_CHANNEL2_SETEVENT         TIMX_SETEVENT(2)
#define TIMX_CHANNEL3_SETEVENT         TIMX_SETEVENT(3)
#define TIMX_CHANNEL4_SETEVENT         TIMX_SETEVENT(4)
#define TIMX_CHANNEL1_GETFLAG          LL_TIM_IsActiveFlag_CC1
#define TIMX_CHANNEL2_GETFLAG          LL_TIM_IsActiveFlag_CC2
#define TIMX_CHANNEL3_GETFLAG          LL_TIM_IsActiveFlag_CC3
#define TIMX_CHANNEL4_GETFLAG          LL_TIM_IsActiveFlag_CC4

#ifdef __cplusplus
 }
#endif

#endif /* USBPD_DEVICE_CONF_H */

