#include "state_machine.h"
#include "power_stage.h"
#include "main.h"

/*
 * Application-level state machine.
 *
 * The regulator's own state machine (INIT → IDLE → RUNNING → FAULT) is
 * fully managed inside power_stage.c.  This module is a thin coordinator
 * that initialises the power stage and monitors its status from the default
 * FreeRTOS task context.
 *
 * Actual voltage/current regulation is interrupt-driven (TIM6 slope comp,
 * HRTIM period, TIM7 PID) and requires no polling from this task.
 */

typedef enum {
    STATE_INIT    = 0,
    STATE_IDLE    = 1,
    STATE_RUNNING = 2,
    STATE_ERROR   = 3,
} AppState_t;

static AppState_t s_app_state = STATE_INIT;

void StateMachine_Init(void)
{
    s_app_state = STATE_INIT;
}

void StateMachine_Task(void)
{
    switch (s_app_state)
    {
        case STATE_INIT:
            /* Initialise the power stage hardware (post-CubeMX MX_* calls). */
            PS_Init();
            s_app_state = STATE_IDLE;
            break;

        case STATE_IDLE:
            /* Power stage is IDLE — waiting for the USB PD stack to negotiate
             * a contract and call regulator_set_target_voltage() + PS_Start().
             * Poll for unexpected fault transitions. */
            if (PS_GetState() == PS_STATE_FAULT) {
                s_app_state = STATE_ERROR;
            } else if (PS_GetState() == PS_STATE_RUNNING) {
                s_app_state = STATE_RUNNING;
            }
            break;

        case STATE_RUNNING:
            /* Regulation is fully interrupt-driven; nothing to do here except
             * monitor for fault conditions. */
            if (PS_GetState() == PS_STATE_FAULT) {
                s_app_state = STATE_ERROR;
            } else if (PS_GetState() == PS_STATE_IDLE) {
                /* Regulator stopped (e.g. 0 V setpoint from PD stack). */
                s_app_state = STATE_IDLE;
            }
            break;

        case STATE_ERROR:
            /* Fault is latched. The USB PD stack is expected to detect the
             * fault via regulator_fault, disconnect VBUS, and call
             * regulator_clear_fault() once the hardware fault clears.
             * Transition back to IDLE when the power stage clears itself. */
            if (PS_GetState() == PS_STATE_IDLE) {
                s_app_state = STATE_IDLE;
            }
            break;

        default:
            s_app_state = STATE_ERROR;
            break;
    }
}