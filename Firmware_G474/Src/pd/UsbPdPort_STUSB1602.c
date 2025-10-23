#include "pd/UsbPdPort.h"

// Placeholder for I2C-based STUSB1602 control; integrate ST middleware for production.

static pd_contract_cb_t s_cb = 0;

void USBPD_Port_Init(pd_contract_cb_t cb) {
    s_cb = cb;
    // TODO: initialize I2C, probe TCPC, program default PDOs
}

void USBPD_Port_RequestPDOs(const uint32_t *src_pdos, uint8_t count) {
    (void)src_pdos; (void)count;
    // TODO: write PDOs into TCPC registers
}

void USBPD_Port_Task(void) {
    // TODO: poll TCPC status, handle attach, negotiation, hard reset, etc.
    // For now, simulate a 20V/3A contract once after attach
    static int once = 0;
    if (!once && s_cb) {
        once = 1;
        s_cb(20.0f, 3.0f);
    }
}
