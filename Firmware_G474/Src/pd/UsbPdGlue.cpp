#include "pd/UsbPdGlue.h"
#include "app/Application.h"

extern "C" void PD_ContractEstablished(uint8_t port, uint32_t mv, uint32_t ma) {
    (void)port;
    App::OnPdContract(mv / 1000.0f, ma / 1000.0f);
}

extern "C" void PD_SourceDisabled(uint8_t port) {
    (void)port;
    // Optional: reduce targets to a safe idle or disable PWM
    App::OnPdContract(5.0f, 0.5f); // fallback low-power target
}
