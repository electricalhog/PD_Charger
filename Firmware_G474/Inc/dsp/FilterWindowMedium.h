/********************************************************************************
 * class        DSP window filter with medium sampling                          *
 * author       @RedCommissary                                                  *
 ********************************************************************************/

#pragma once

#include "FastSort.h"

class FilterWindowMedium {
public:
    static float Compute (uint16_t *array, uint16_t size, uint16_t window) {
        FastSort::Recursive(array, size);

        float dataBuffer = 0.0f;
        uint16_t highLimit = (size / 2) + (window / 2);
        uint16_t lowLimit  = (size / 2) - (window / 2);
        for (uint16_t i = lowLimit; i < highLimit; i++) {
            dataBuffer += array[i];
        }
        return (static_cast<float>(dataBuffer / window));
    }
};
