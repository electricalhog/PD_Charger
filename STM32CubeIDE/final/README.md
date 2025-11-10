Build and flash outside STM32CubeIDE

Prereqs
- GNU Arm Embedded Toolchain in PATH (arm-none-eabi-gcc, objcopy, etc.)
- One flasher:
  - STM32CubeProgrammer CLI (STM32_Programmer_CLI) in PATH, or
  - OpenOCD in PATH

Build
- In a Bash shell:
  cd STM32CubeIDE/final
  make -j

Artifacts are in STM32CubeIDE/final/build:
- final.elf, final.hex, final.bin, final.map

Flash
- Default uses STM32CubeProgrammer:
  make flash
- To use a specific probe serial:
  make flash STLINK_SN=YOUR_SERIAL
- To use OpenOCD instead:
  make flash FLASH_TOOL=openocd

Clean
  make clean

Notes
- The Makefile mirrors the include paths, defines, and linker script from the CubeIDE project to stay compatible.
- USB-PD core is linked from the prebuilt library at Middlewares/ST/STM32_USBPD_Library/Core/lib.
