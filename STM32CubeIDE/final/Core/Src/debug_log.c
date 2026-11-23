/**
 * @file    debug_log.c
 * @brief   High-speed circular sample buffer implementation (§15.3).
 *
 * See debug_log.h for overview, buffer layout, and usage.
 *
 * NLSpec conformance: v0.1.2j §15.3
 */

#include "debug_log.h"
#include "stm32g4xx_hal.h"  /* HAL_NVIC_DisableIRQ / EnableIRQ, TIM7_DAC_IRQn */
#include <string.h>         /* memset */

/* =========================================================================
 * Global instance — readable directly in the debugger as "debug_log"
 * =========================================================================*/

DebugLog debug_log;

/* =========================================================================
 * debug_log_record
 * =========================================================================*/

/**
 * Append one DebugSample to the circular buffer.
 *
 * Called exclusively from the TIM7 ISR (PID, priority 2).  No locking is
 * needed because only one execution context writes to the buffer.
 */
void debug_log_record(const DebugSample *sample)
{
    if (sample == NULL)
    {
        return;
    }

    debug_log.samples[debug_log.write_index] = *sample;

    debug_log.write_index++;
    if (debug_log.write_index >= DEBUG_LOG_SIZE)
    {
        debug_log.write_index = 0u;
        debug_log.wrapped     = 1u;
    }

    debug_log.total_count++;
}

/* =========================================================================
 * debug_log_clear
 * =========================================================================*/

/**
 * Zero the buffer and reset all indices.
 *
 * Uses HAL_NVIC_DisableIRQ / EnableIRQ rather than a FreeRTOS critical
 * section because the TIM7 ISR runs at priority 2, which is above
 * configMAX_SYSCALL_INTERRUPT_PRIORITY (5).  FreeRTOS critical sections
 * (taskENTER_CRITICAL) use BASEPRI to mask ISRs up to that priority, so
 * they cannot mask TIM7.  Direct NVIC enable/disable correctly gates a
 * single IRQ regardless of priority.
 *
 * May be called from any FreeRTOS task context.  Must NOT be called from
 * an ISR.
 */
void debug_log_clear(void)
{
    HAL_NVIC_DisableIRQ(TIM7_DAC_IRQn);
    memset(&debug_log, 0, sizeof(debug_log));
    HAL_NVIC_EnableIRQ(TIM7_DAC_IRQn);
}
