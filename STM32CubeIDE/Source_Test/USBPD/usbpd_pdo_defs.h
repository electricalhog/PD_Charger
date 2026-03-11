/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbpd_pdo_defs.h
  * @author  MCD Application Team
  * @brief   Header file for definition of PDO/APDO values for 2 ports(DRP/SNK) configuration
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

#ifndef __USBPD_PDO_DEF_H_
#define __USBPD_PDO_DEF_H_

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usbpd_def.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Define   ------------------------------------------------------------------*/
#define PORT0_NB_SOURCEPDO         5U   /* Number of Source PDOs (applicable for port 0) — SPR: 5V,9V,12V,15V,20V */
#define PORT0_NB_SINKPDO           0U   /* Number of Sink PDOs (applicable for port 0)     */
#define PORT1_NB_SOURCEPDO         0U   /* Number of Source PDOs (applicable for port 1)   */
#define PORT1_NB_SINKPDO           0U   /* Number of Sink PDOs (applicable for port 1)     */

/* USER CODE BEGIN Define */

/* USER CODE END Define */

/* Exported typedef ----------------------------------------------------------*/

/* USER CODE BEGIN typedef */
/**
  * @brief  USBPD Port PDO Structure definition
  *
  */

 /**
   * @brief  USBPD Port PDO Structure definition
   */
 typedef struct
 {
   uint32_t *ListOfPDO;                          /*!< Pointer on Power Data Objects list, defining port capabilities */
   uint8_t  *NumberOfPDO;                        /*!< Number of Power Data Objects defined in ListOfPDO
                                                 This parameter must be set at max to @ref USBPD_MAX_NB_PDO value */
 } USBPD_PortPDO_TypeDef;

 /**
   * @brief  USBPD Port PDO Storage Structure definition
   */

typedef struct
{
  USBPD_PortPDO_TypeDef    SourcePDO;            /*!< SRC Power Data Objects */
  USBPD_PortPDO_TypeDef    SinkPDO;              /*!< SNK Power Data Objects */

} USBPD_PWR_Port_PDO_Storage_TypeDef;
/* USER CODE END typedef */

/* Exported define -----------------------------------------------------------*/

/* USER CODE BEGIN Exported_Define */

#define USBPD_CORE_PDO_SRC_FIXED_MAX_CURRENT 3000  /* 3 A for SPR PDOs 1–4 */
#define USBPD_CORE_PDO_SNK_FIXED_MAX_CURRENT 1500

/* USER CODE END Exported_Define */

/* Exported constants --------------------------------------------------------*/

/* USER CODE BEGIN constants */

/* USER CODE END constants */

/* Exported macro ------------------------------------------------------------*/

/* USER CODE BEGIN macro */

/* USER CODE END macro */

/* Exported variables --------------------------------------------------------*/

/* USER CODE BEGIN variables */

#ifndef _GUI_INTERFACE
#ifndef __USBPD_PWR_IF_C
extern uint8_t USBPD_NbPDO[4];
extern uint32_t PORT0_PDO_ListSRC[USBPD_MAX_NB_PDO];
extern uint32_t PORT0_PDO_ListSNK[USBPD_MAX_NB_PDO];
#else /* __USBPD_PWR_IF_C */
uint8_t USBPD_NbPDO[4] = {(PORT0_NB_SINKPDO),
                          (PORT0_NB_SOURCEPDO)};
#endif /* __USBPD_PWR_IF_C */
#endif /* _GUI_INTERFACE */

/* USER CODE END variables */

#ifndef __USBPD_PWR_IF_C
extern uint8_t USBPD_NbPDO[4];
extern uint32_t PORT0_PDO_ListSRC[USBPD_MAX_NB_PDO];
extern uint32_t PORT0_PDO_ListSNK[USBPD_MAX_NB_PDO];
#else
uint8_t USBPD_NbPDO[4] = {(PORT0_NB_SINKPDO),
                          (PORT0_NB_SOURCEPDO)};
/* Definition of Source PDO for Port 0 */
uint32_t PORT0_PDO_ListSRC[USBPD_MAX_NB_PDO] =
{
  /*
   * Source PDO table — USB PD Standard Power Range (SPR) voltages.
   * NLSpec §9.4, firmware_plan §4a.
   * All PDOs use fixed-voltage format (USBPD_PDO_TYPE_FIXED).
   * PDO 1 (mandatory 5 V) carries the capability flags for the port.
   */

  /* PDO 1 — 5 V @ 3 A (mandatory; carries port capability flags) */
  (
    USBPD_PDO_TYPE_FIXED                              |
    USBPD_PDO_SRC_FIXED_SET_VOLTAGE(5000U)            |  /* 5000 mV */
    USBPD_PDO_SRC_FIXED_SET_MAX_CURRENT(3000U)        |  /* 3000 mA */
    USBPD_PDO_SRC_FIXED_PEAKCURRENT_EQUAL             |
    USBPD_PDO_SRC_FIXED_UNCHUNK_NOT_SUPPORTED         |
    USBPD_PDO_SRC_FIXED_DRD_SUPPORTED                 |
    USBPD_PDO_SRC_FIXED_USBCOMM_NOT_SUPPORTED         |
    USBPD_PDO_SRC_FIXED_EXT_POWER_NOT_AVAILABLE       |
    USBPD_PDO_SRC_FIXED_USBSUSPEND_NOT_SUPPORTED      |
    USBPD_PDO_SRC_FIXED_DRP_NOT_SUPPORTED
  ),

  /* PDO 2 — 9 V @ 3 A */
  (
    USBPD_PDO_TYPE_FIXED                              |
    USBPD_PDO_SRC_FIXED_SET_VOLTAGE(9000U)            |
    USBPD_PDO_SRC_FIXED_SET_MAX_CURRENT(3000U)        |
    USBPD_PDO_SRC_FIXED_PEAKCURRENT_EQUAL
  ),

  /* PDO 3 — 12 V @ 3 A */
  (
    USBPD_PDO_TYPE_FIXED                              |
    USBPD_PDO_SRC_FIXED_SET_VOLTAGE(12000U)           |
    USBPD_PDO_SRC_FIXED_SET_MAX_CURRENT(3000U)        |
    USBPD_PDO_SRC_FIXED_PEAKCURRENT_EQUAL
  ),

  /* PDO 4 — 15 V @ 3 A */
  (
    USBPD_PDO_TYPE_FIXED                              |
    USBPD_PDO_SRC_FIXED_SET_VOLTAGE(15000U)           |
    USBPD_PDO_SRC_FIXED_SET_MAX_CURRENT(3000U)        |
    USBPD_PDO_SRC_FIXED_PEAKCURRENT_EQUAL
  ),

  /* PDO 5 — 20 V @ 5 A (maximum SPR power = 100 W) */
  (
    USBPD_PDO_TYPE_FIXED                              |
    USBPD_PDO_SRC_FIXED_SET_VOLTAGE(20000U)           |
    USBPD_PDO_SRC_FIXED_SET_MAX_CURRENT(5000U)        |
    USBPD_PDO_SRC_FIXED_PEAKCURRENT_EQUAL
  ),

  /* PDO 6 — reserved (EPR AVS 28 V when EPR library available) */
  (0x00000000U),

  /* PDO 7 — reserved (EPR AVS 36 V / 48 V when EPR library available) */
  (0x00000000U),

};

/* Definition of Sink PDO for Port 0 */
uint32_t PORT0_PDO_ListSNK[USBPD_MAX_NB_PDO] =
{

  /* PDO 1 */ (0x00000000U),

  /* PDO 2 */ (0x00000000U),

  /* PDO 3 */ (0x00000000U),

  /* PDO 4 */ (0x00000000U),

  /* PDO 5 */ (0x00000000U),

  /* PDO 6 */ (0x00000000U),

  /* PDO 7 */ (0x00000000U),
};

#endif

/* Exported functions --------------------------------------------------------*/

/* USER CODE BEGIN functions */

/* USER CODE END functions */

#ifdef __cplusplus
}
#endif

#endif /* __USBPD_PDO_DEF_H_ */
