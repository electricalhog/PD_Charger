#pragma once

#include <stdint.h>
#include "board/Board.h"
#include "dsp/FilterWindowMedium.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "stm32g4xx_hal.h"
extern ADC_HandleTypeDef hadc1; // Provided by CubeMX project
#ifdef __cplusplus
}
#endif

namespace AdcIf {
    constexpr uint16_t kBufSize = 64;
    extern volatile uint16_t bufVin[kBufSize];
    extern volatile uint16_t bufIin[kBufSize];
    extern volatile uint16_t bufVout[kBufSize];
    extern volatile uint16_t bufIout[kBufSize];

    void InitAndStart();

    // Filtered, scaled readings
    float GetVin();
    float GetIin();
    float GetVout();
    float GetIout();
}
