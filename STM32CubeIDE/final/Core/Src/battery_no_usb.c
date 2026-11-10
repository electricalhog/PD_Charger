/**
 * @file    battery_no_usb.c
 * @brief   Battery-load build (BATTERY_LOAD, plans/lipo-load.md): no USB.
 *
 * A LiPo sits on VBUS as a power sink, so the USB-PD stack, UCPD, the
 * TCPP0203 code and the UCPD tracer are left out of the build (CMakeLists.txt).
 * CubeMX's generated code still calls a few of their entry points outside
 * USER CODE; they are defined here as no-ops.  The UCPD peripheral is never
 * enabled, so the CC pins present no Rp and nothing can attach.
 */

#include "main.h"

#if defined(BATTERY_LOAD) && (BATTERY_LOAD)

/* main.c: the TCPP0203 (X-NUCLEO-SRC1M1) init; there is no shield. */
void MX_TCPP_Init(void) {}

/* main.c: the USB-PD stack's init.  Its IRQ was enabled by MX_UCPD1_Init. */
void MX_USBPD_Init(void)
{
    NVIC_DisableIRQ(UCPD1_IRQn);
}

/* stm32g4xx_it.c: UCPD1, and the tracer's DMA and LPUART interrupts. */
void USBPD_PORT0_IRQHandler(void) {}
void TRACER_EMB_IRQHandlerDMA(void) {}
void TRACER_EMB_IRQHandlerUSART(void) {}

#endif
