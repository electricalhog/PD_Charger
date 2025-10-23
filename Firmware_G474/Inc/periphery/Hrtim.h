#pragma once

#include <stdint.h>
#include "board/Board.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "stm32g4xx_hal.h"
#ifdef __cplusplus
}
#endif

class HrtimDriver {
public:
    // Initialize HRTIM with two complementary timers (A=Boost, B=Buck)
    static void Init(uint32_t fsw_hz = BoardCfg::kFsw_Hz, uint32_t deadtime_ns = BoardCfg::kDeadtime_ns);

    // Set duty (0..period) for a leg; automatically handles complementary outputs
    static void SetDuty(BoardCfg::Leg leg, uint16_t duty);

    // Get timer period in ticks
    static uint16_t PeriodTicks();

    // Provide access to handle for ADC trigger configuration
    static HRTIM_HandleTypeDef* Handle();

private:
    static HRTIM_HandleTypeDef hhrtim1_;
    static uint16_t period_;
};
