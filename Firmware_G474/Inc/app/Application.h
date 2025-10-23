#pragma once

#include <stdint.h>
#include "periphery/Hrtim.h"
#include "periphery/AdcIf.h"
#include "dsp/Pid.h"

namespace App {
    struct Targets {
        float vout_V{20.0f};
        float iout_A{3.0f};
    };

    enum class Mode : uint8_t { Boost, BuckBoost, Buck };
    enum class State : uint8_t { Init, WaitPd, SoftStart, Run, Fault };

    void Init();
    void OnControlTick(); // called at control rate (e.g., 20 kHz) from timer/ISR
    void OnLowTick();     // 1 kHz slow tasks

    void SetTargets(const Targets &t);
    void OnPdContract(float v_V, float i_A); // called by PD layer when a contract is ready
}
