/**
 * @file    debug_log.h
 * @brief   High-speed circular sample buffer for in-circuit debug capture (§15.3).
 *
 * Overview
 * --------
 * Because LPUART1 is claimed by the UCPD peripheral it cannot be used for
 * serial telemetry during closed-loop operation.  Instead this module
 * maintains a 512-sample circular buffer in SRAM that is written once per PID
 * cycle (20 kHz) from the TIM7 ISR.  The buffer can be inspected without
 * halting the CPU using any JTAG/SWD debugger that supports live memory
 * viewing (e.g. the STM32CubeIDE "Memory" or "Expressions" windows,
 * ST-Link live data viewer, or a custom GDB script).
 *
 * Buffer layout
 * -------------
 * @code
 * debug_log.samples[0..511]   — DebugSample ring
 * debug_log.write_index        — next write position (wraps at DEBUG_LOG_SIZE)
 * debug_log.total_count        — monotonically increasing sample counter
 * debug_log.wrapped            — non-zero once the buffer has wrapped once
 * @endcode
 *
 * Reading the buffer in a debug session
 * --------------------------------------
 * 1. Pause the target (or use live-variable view — no pause needed).
 * 2. Add "debug_log" as a watched expression in the Expressions window, or
 *    open Memory view at the address of "debug_log".
 * 3. Note debug_log.write_index: samples [0 .. write_index-1] are the most
 *    recent data if wrapped == 0; if wrapped != 0 the oldest sample is at
 *    write_index (ring order).
 * 4. Export or copy the samples array for offline DSP analysis.
 *    At 20 kHz, 512 samples = 25.6 ms of continuous capture.
 *
 * Concurrency
 * -----------
 * debug_log_record() is called exclusively from the TIM7 ISR; no locking is
 * required.  debug_log_clear() may be called from the FreeRTOS task context;
 * it disables the TIM7 interrupt briefly to ensure an atomic clear.
 *
 * NLSpec conformance: v0.1.2j §15.3
 */

#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Buffer sizing
 * =========================================================================*/

/**
 * DEBUG_LOG_SIZE — Number of DebugSample entries in the circular buffer.
 * At 20 kHz this gives 512 / 20 000 = 25.6 ms of continuous capture.
 * Memory cost: 512 × 24 bytes = 12 288 bytes (~12 KB SRAM).
 * Increase up to 1024 (24 KB) if more capture depth is needed; the
 * STM32G474 has 128 KB SRAM total.
 */
#define DEBUG_LOG_SIZE 512u

/* =========================================================================
 * Sample structure
 * =========================================================================*/

/**
 * DebugSample — one PID-cycle snapshot of regulator state.
 *
 * All fields are in "engineering" units (mV, mA) so they can be read
 * directly in the debugger without scaling.  Total size: 24 bytes.
 *
 * TODO(debug): Quick triage guide for debug_log inspection:
 *   - If v_out_mv is always 0    → check ADC1 DMA init, INPUT_EN assert.
 *   - If v_in_mv  is always 0    → check ADC2 trigger / INPUT_EN (§5.1).
 *   - If dac_counts stays at max → PID saturated; verify setpoint & gains.
 *   - If state == 3 (FAULT)      → check regulator_last_fault_source for
 *                                   cause (OVP/UVP/VIN_RANGE/HW_FLTx).
 *   - If mode == 1 in buck HW    → v_in_mv=0 at start caused wrong mode;
 *                                   fixed by regulator_start() ADC pre-sample.
 *   - If total_count never grows → TIM7 ISR not firing; check TIM7_DAC_IRQn
 *                                   NVIC priority and vector name.
 */
typedef struct
{
    uint32_t v_out_mv;       /**< Output voltage         [mV] */
    uint32_t v_in_mv;        /**< Input voltage          [mV] */
    uint32_t i_inductor_ma;  /**< Inductor current       [mA] (unsigned; sign via mode) */
    uint32_t i_out_ma;       /**< Output current         [mA] */
    uint16_t dac_counts;     /**< PID output — DAC3 CH1 counts [0..4095] */
    uint8_t  state;          /**< RegulatorState (0=INIT,1=IDLE,2=RUNNING,3=FAULT) */
    uint8_t  mode;           /**< RegulatorMode  (0=BUCK,1=BOOST,2=BUCK_BOOST) */
    int32_t  error_mv;       /**< PID error: setpoint − V_out [mV] */
} DebugSample;

/* =========================================================================
 * Buffer type
 * =========================================================================*/

/**
 * DebugLog — circular buffer control block.
 *
 * Declared extern; the single instance "debug_log" lives in debug_log.c.
 * Inspecting this struct in a debugger gives full access to all captured
 * samples plus the write cursor and overflow indicator.
 */
typedef struct
{
    DebugSample samples[DEBUG_LOG_SIZE];  /**< Circular sample ring */
    uint32_t    write_index;             /**< Next write slot (mod DEBUG_LOG_SIZE) */
    uint32_t    total_count;             /**< Total samples written (never wraps 32-bit at 20 kHz) */
    uint8_t     wrapped;                 /**< 1 once the ring has wrapped; 0 before first wrap */
    uint8_t     _pad[3];                 /**< Alignment padding */
} DebugLog;

/* =========================================================================
 * Public API
 * =========================================================================*/

/** Global debug log instance — read directly in the debugger. */
extern DebugLog debug_log;

/**
 * debug_log_record — Append one sample to the circular buffer.
 *
 * Called from the TIM7 PID ISR (priority 2) on every PID cycle.
 * Executes in ≤10 CPU cycles (one struct copy + two increments).
 * Must NOT be called from higher-priority ISRs.
 *
 * @param sample  Pointer to the sample to record; contents are copied.
 */
void debug_log_record(const DebugSample *sample);

/**
 * debug_log_clear — Zero the buffer and reset all indices.
 *
 * May be called from the FreeRTOS task context to start a fresh capture.
 * Disables the TIM7 interrupt briefly to ensure an atomic clear.
 */
void debug_log_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_LOG_H */
