#include "stm32g4xx_hal.h"
#include "app/Application.h"
#include "pd/UsbPdPort.h"

ADC_HandleTypeDef hadc1; // provided by CubeMX in real project
TIM_HandleTypeDef htim6; // control-rate timer (configure in CubeMX)
TIM_HandleTypeDef htim7; // low-rate timer (1 kHz)

static void SystemClock_Config(void);
static void MX_ADC1_Init(void);
static void MX_TIM6_Init(void);
static void MX_TIM7_Init(void);

static void on_pd_contract(float v, float i) {
    App::OnPdContract(v, i);
}

int main(void) {
    HAL_Init();
    SystemClock_Config();

    MX_ADC1_Init();
    MX_TIM6_Init();
    MX_TIM7_Init();

    App::Init();
    USBPD_Port_Init(on_pd_contract);

    HAL_TIM_Base_Start_IT(&htim6); // control tick
    HAL_TIM_Base_Start_IT(&htim7); // 1 kHz tick

    while (1) {
        USBPD_Port_Task();
        // Other background tasks
    }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
    if (htim->Instance == TIM6) {
        App::OnControlTick();
    } else if (htim->Instance == TIM7) {
        App::OnLowTick();
    }
}

// The following init functions are placeholders. Use CubeMX to generate real init code.
static void SystemClock_Config(void) {}
static void MX_ADC1_Init(void) {}
static void MX_TIM6_Init(void) {}
static void MX_TIM7_Init(void) {}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) { (void)file; (void)line; }
#endif
