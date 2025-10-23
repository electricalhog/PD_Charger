#include "periphery/AdcIf.h"

namespace AdcIf {
    volatile uint16_t bufVin[kBufSize]  = {0};
    volatile uint16_t bufIin[kBufSize]  = {0};
    volatile uint16_t bufVout[kBufSize] = {0};
    volatile uint16_t bufIout[kBufSize] = {0};

    static uint16_t window = 20; // median window size

    void InitAndStart() {
        // Assumes ADC, DMA, and HRTIM trigger are configured in CubeMX
        HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED);
        HAL_ADC_Start_DMA(&hadc1, (uint32_t*)bufVin, kBufSize); // example; adapt to multi-channel DMA setup
    }

    static float adc_to_volts(uint16_t raw, float div) {
        return (raw * BoardCfg::Adc::lsb * div);
    }
    static float adc_to_amps(uint16_t raw) {
        return (raw * BoardCfg::Adc::lsb / (BoardCfg::Adc::kIgain * BoardCfg::Adc::kIshunt));
    }

    float GetVin() {
        // Copy to a temp buffer (non-volatile) then filter
        uint16_t tmp[kBufSize];
        for (uint16_t i = 0; i < kBufSize; ++i) tmp[i] = bufVin[i];
        float mv = FilterWindowMedium::Compute(tmp, kBufSize, window);
        return adc_to_volts((uint16_t)mv, BoardCfg::Adc::kVinDiv);
    }

    float GetIin() {
        uint16_t tmp[kBufSize];
        for (uint16_t i = 0; i < kBufSize; ++i) tmp[i] = bufIin[i];
        float mv = FilterWindowMedium::Compute(tmp, kBufSize, window);
        return adc_to_amps((uint16_t)mv);
    }

    float GetVout() {
        uint16_t tmp[kBufSize];
        for (uint16_t i = 0; i < kBufSize; ++i) tmp[i] = bufVout[i];
        float mv = FilterWindowMedium::Compute(tmp, kBufSize, window);
        return adc_to_volts((uint16_t)mv, BoardCfg::Adc::kVoutDiv);
    }

    float GetIout() {
        uint16_t tmp[kBufSize];
        for (uint16_t i = 0; i < kBufSize; ++i) tmp[i] = bufIout[i];
        float mv = FilterWindowMedium::Compute(tmp, kBufSize, window);
        return adc_to_amps((uint16_t)mv);
    }
}
