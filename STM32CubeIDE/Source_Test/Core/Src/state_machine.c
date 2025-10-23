#include "state_machine.h"
#include "main.h"

// Define states
typedef enum {
    STATE_INIT,
    STATE_MEASURE,
    STATE_REGULATE,
    STATE_CALCULATE_DUTY,
    STATE_ERROR
} State_t;

typedef enum {
    NONE,
    BUCK,
    BOOST,
    BUCKBOOST,
    ERROR
} RegulationType_t;

static State_t currentState = STATE_INIT;

static RegulationType_t regulationType = NONE;

// Define voltage thresholds
#define BUCK_VOLTAGE_THRESHOLD 1.33f
#define BUCK_BUCKBOOST_VOLTAGE_THRESHOLD 1.18f
#define BOOST_BUCKBOOST_VOLTAGE_THRESHOLD 0.85f
#define BOOST_VOLTAGE_THRESHOLD 0.75f

// Example variables
static float outputVoltage = 0.0f;
static float inputVoltage = 0.0f;
static float targetVoltage = 5.0f;
static float dutyCycle = 0.0f;

void StateMachine_Init(void) {
    currentState = STATE_INIT;
}

void StateMachine_Task(void) {
    switch (currentState) {
        case STATE_INIT:
            // Initialization logic
            currentState = STATE_MEASURE;
            break;

        case STATE_MEASURE:
            // Perform measurement
            outputVoltage = 3.3f; // Example
            currentState = STATE_REGULATE;
            break;

        case STATE_REGULATE:
            // Determine regulation type
            switch (regulationType) { // TODO: make discrete fumction
                case NONE:
                    // No regulation needed
                    break;
                case BUCK:
                    // Buck mode
                    if ((inputVoltage / outputVoltage) < BOOST_VOLTAGE_THRESHOLD) {
                        // Switch to boost mode
                        regulationType = BOOST;
                        // Buck mode config
                    }
                    else if ((inputVoltage / outputVoltage) < BUCK_BUCKBOOST_VOLTAGE_THRESHOLD) {
                        // Switch to buck-boost mode
                        regulationType = BUCKBOOST;
                        // Buck-boost mode config
                    }
                    break;
                case BOOST:
                    // Boost mode
                    if ((inputVoltage / outputVoltage) > BUCK_VOLTAGE_THRESHOLD) {
                        // Switch to buck mode
                        regulationType = BUCK;
                        // Boost mode config
                    }
                    else if ((inputVoltage / outputVoltage) > BOOST_BUCKBOOST_VOLTAGE_THRESHOLD) {
                        // Switch to buck-boost mode
                        regulationType = BUCKBOOST;
                        // Buck-boost mode config
                    }
                    break;
                case BUCKBOOST:
                    // Buck-boost mode
                    if ((inputVoltage / outputVoltage) > BUCK_VOLTAGE_THRESHOLD) {
                        // Switch to buck mode
                        regulationType = BUCK;
                        // Buck-boost mode config
                    }
                    else if ((inputVoltage / outputVoltage) < BOOST_VOLTAGE_THRESHOLD) {
                        // Switch to boost mode
                        regulationType = BOOST;
                        // Buck-boost mode config
                    }
                    break;
                case ERROR:
                    // Handle error
                    //TODO: what is an error?
                    break;
            }
            currentState = STATE_CALCULATE_DUTY;
            break;

        case STATE_CALCULATE_DUTY:
            // Calculate duty cycle
            // TODO: PID control logic
            PID.calculate(outputVoltage, targetVoltage);
            dutyCycle += PID.error;
            if (dutyCycle > 100.0f) {
                dutyCycle = 100.0f;
            } else if (dutyCycle < 0.0f) {
                dutyCycle = 0.0f;
            }
            break;

        case STATE_ERROR:
            // Handle error
            Error_Handler();
            break;

        default:
            currentState = STATE_ERROR;
            break;
    }
}