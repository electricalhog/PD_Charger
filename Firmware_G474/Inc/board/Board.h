#pragma once

#include <stdint.h>

// Board- and project-wide configuration
// Map HRTIM outputs and ADC channels for NUCLEO-G474RE + external power stage

namespace BoardCfg {
    // PWM switching frequency in Hz
    constexpr uint32_t kFsw_Hz = 200000; // 200 kHz

    // Deadtime in nanoseconds (both rising and falling)
    constexpr uint32_t kDeadtime_ns = 100; // tune per driver

    // ADC reference and scaling
    namespace Adc {
        constexpr float vref = 3.3f;
        constexpr uint32_t resolution = 4096; // 12-bit
        constexpr float lsb = vref / static_cast<float>(resolution);

        // Sensing network
        constexpr float kVinDiv = 4.9f;   // Vin divider ratio (F3 example)
        constexpr float kVoutDiv = 4.9f;  // Vout divider ratio
        constexpr float kIshunt = 0.02f;  // 20 mΩ shunt
        constexpr float kIgain  = 50.0f;  // current amplifier gain
    }

    // HRTIM timers assignment
    // Timer A: Boost leg complementary outputs (TA1/TA2)
    // Timer B: Buck leg complementary outputs  (TB1/TB2)
    enum class Leg : uint8_t { Boost = 0, Buck = 1 };
}
