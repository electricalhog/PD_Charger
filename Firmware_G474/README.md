STM32G474 USB‑PD Buck‑Boost Source (240 W)

Overview

This firmware targets the NUCLEO‑G474RE driving a 4‑switch buck‑boost power stage with complementary gate signals and programmable deadtime using HRTIM. It integrates with an external USB Type‑C PD controller (TCPC) such as X‑NUCLEO‑SRC1M1 (STUSB1602) to negotiate source power profiles (PDOs) and set the DC/DC output accordingly.

Key features

- Complementary HRTIM PWM for two half‑bridges (buck and boost legs) with independent deadtime
- ADC sampling synchronized to PWM edges; filtered measurements for Vin, Iin, Vout, Iout
- Mode control: boost, buck‑boost, buck based on Vin vs target Vout
- PID control (voltage‑mode with optional current loop)
- Safety: UVLO/OVLO, overcurrent, temperature (hooks provided)
- USB‑PD source via UCPD middleware (CubeMX UCPD Configurator for PDOs) or via external TCPC (STUSB1602) — both supported stubs

Repository layout

- Inc/
  - app/: High‑level application and control logic
  - periphery/: HRTIM and ADC/DMA interfaces
  - pd/: USB‑PD TCPC abstraction
  - board/: Pin mapping and HW configuration
  - dsp/: PID and filters reused from the F3 example
- Src/: Implementation files (mirrors Inc/)
- Makefile: Optional build file if you add HAL/USBPD sources manually; otherwise import into STM32CubeIDE

Getting started

1) Generate a CubeMX project for NUCLEO‑G474RE with:
   - HRTIM1: Two timers for complementary PWM (Timer A = Boost leg, Timer B = Buck leg), enable deadtime, set frequency (e.g., 200 kHz)
   - ADC1 (and ADC2 if needed): Regular conversions with DMA; trigger from HRTIM TRG
  - Option A: UCPD1 as Source, enable USB‑PD Core/Middleware; open the UCPD Configurator and define Source PDOs (SPR/PPS/EPR as applicable)
  - Option B: I2C (I2C1) for external TCPC (STUSB1602 on X‑NUCLEO‑SRC1M1)
   - GPIO: Map HRTIM outputs to your gate driver inputs; map BACKUP/FAULT pins as needed
2) Copy this folder’s Inc/ and Src/ into the Cube project (e.g., Core/Inc and Core/Src or keep as separate groups).
3) If using UCPD (recommended):
  - CubeMX will generate usbpd_* sources (usbpd_dpm_user.c, usbpd_pdo_defs.c, usbpd_pwr_if.c, etc.)
  - In `usbpd_dpm_user.c`, when a Source contract is completed (e.g., in `USBPD_DPM_Notification` on `USBPD_NOTIFY_SOURCE_CONTRACT`), call:
    `PD_ContractEstablished(PORT0, RequestedVoltage_mV, RequestedCurrent_mA);`
    where `RequestedVoltage_mV` and `RequestedCurrent_mA` can be read from the DPM context or negotiated request. The glue is provided in `Inc/pd/UsbPdGlue.h`.
  - On detach or error, call `PD_SourceDisabled(port)`.
4) If using external TCPC: add ST’s driver/middleware and replace the stub in `Src/pd/UsbPdPort_STUSB1602.c`.
4) Wire X‑NUCLEO‑SRC1M1 to I2C and VBUS path; connect your 4‑switch power stage to the assigned HRTIM pins.
5) Build and flash. On PD contract, the app will set the DC/DC to the negotiated voltage and current limit.

Quick build (optional, Makefile)

If you maintain a local copy of HAL and USB‑PD sources, export paths and run:

```bash
export HAL_G4_PATH=/path/to/stm32g4xx_hal
export USBPD_PATH=/path/to/x-cube-usb-pd
make -C Firmware_G474
```

Safety note

Start with a current‑limited bench supply and a resistive load. Verify gate timing and deadtime on an oscilloscope before enabling high power. Tune PID gains at low power and increase gradually.
