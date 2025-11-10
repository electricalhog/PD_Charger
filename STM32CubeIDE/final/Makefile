# Make outside STM32CubeIDE (compatible with IDE layout)
# Toolchain (override from env if needed)
CROSS_COMPILE ?= arm-none-eabi-
CC  := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++
AS  := $(CROSS_COMPILE)gcc
AR  := $(CROSS_COMPILE)ar
LD  := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump
SIZE    := $(CROSS_COMPILE)size

# OS/Shell portability
# On Windows, GNU make may use sh.exe (MSYS/Git Bash) or cmd.exe. Pick commands accordingly.
ifeq ($(OS),Windows_NT)
  ifneq (,$(findstring sh,$(SHELL)))
    MKDIR_P := mkdir -p
    RM_RF   := rm -rf
    RM_F    := rm -f
  else
    MKDIR_P := mkdir
    RM_RF   := rmdir /S /Q
    RM_F    := del /F /Q
  endif
else
  MKDIR_P := mkdir -p
  RM_RF   := rm -rf
  RM_F    := rm -f
endif
 
# External tools (override to provide full path if needed)
STM32_PROGRAMMER ?= STM32_Programmer_CLI
 
.DELETE_ON_ERROR:

# Project
PROJECT := final
BUILD_DIR ?= build
TARGET := $(BUILD_DIR)/$(PROJECT)

# MCU and FPU
CPU := -mcpu=cortex-m4 -mthumb
FPU := -mfpu=fpv4-sp-d16 -mfloat-abi=hard

# Common flags
OPT ?= -O2
C_DEFS := \
  -DUSE_HAL_DRIVER \
  -DSTM32G474xx \
  -DUSE_FULL_LL_DRIVER \
  -DUSBPD_PORT_COUNT=1 \
  -D_RTOS \
  -D_SRC \
  -D_TRACE \
  -DUSBPDCORE_LIB_PD3_FULL \
  -DTCPP0203_SUPPORT \
  -D_GUI_INTERFACE

INCLUDES := \
  -ICore/Inc \
  -IDrivers/STM32G4xx_HAL_Driver/Inc \
  -IDrivers/STM32G4xx_HAL_Driver/Inc/Legacy \
  -IDrivers/CMSIS/Device/ST/STM32G4xx/Include \
  -IDrivers/CMSIS/Include \
  -IUSBPD/App \
  -IUSBPD/Target \
  -IUSBPD \
  -IUtilities/TRACER_EMB \
  -IMiddlewares/Third_Party/FreeRTOS/Source/include \
  -IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS \
  -IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F \
  -IMiddlewares/ST/STM32_USBPD_Library/Core/inc \
  -IMiddlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/inc \
  -ITCPP/App \
  -ITCPP/Target \
  -ITCPP \
  -IDrivers/BSP/X-NUCLEO-SRC1M1 \
  -IDrivers/BSP/Components/tcpp0203 \
  -IUtilities/GUI_INTERFACE

CFLAGS := $(CPU) $(FPU) $(OPT) -ffunction-sections -fdata-sections -fno-common -fmessage-length=0 \
  -Wall -Wextra -Werror-implicit-function-declaration -Wno-unused-parameter -g3 $(C_DEFS) $(INCLUDES)
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti -fno-use-cxa-atexit
ASFLAGS := $(CPU) $(FPU) -x assembler-with-cpp $(C_DEFS) $(INCLUDES)
# USBPD prebuilt core library
USBPD_LIB_DIR := Middlewares/ST/STM32_USBPD_Library/Core/lib
USBPD_LIB     := $(USBPD_LIB_DIR)/USBPDCORE_PD3_FULL_CM4_wc32.a

LDFLAGS := $(CPU) $(FPU) -specs=nano.specs -Wl,--gc-sections -Wl,-Map=$(TARGET).map \
  -T STM32G474RETX_FLASH.ld \
  -L $(USBPD_LIB_DIR)
LDLIBS := $(USBPD_LIB)

# Source discovery (broader, recursive), but exclude USBPD core sources (linked as prebuilt lib)
# Recursive wildcard helper
rwildcard = $(wildcard $1$2) $(foreach d,$(wildcard $1*/),$(call rwildcard,$d,$2))

SRC_DIRS := \
  Core/Src \
  Drivers \
  Middlewares/Third_Party/FreeRTOS/Source \
  Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src \
  USBPD/App \
  USBPD/Target \
  Utilities \
  TCPP/App \
  TCPP/Target

# Collect all C sources under the listed directories
SRC_C := $(foreach d,$(SRC_DIRS),$(call rwildcard,$(d)/,*.c))

# Exclude USBPD core sources (we link the archive instead)
SRC_C := $(filter-out Middlewares/ST/STM32_USBPD_Library/Core/src/%,$(SRC_C))

# Re-include USBPD trace source explicitly (not part of the prebuilt core archive)
SRC_C += Middlewares/ST/STM32_USBPD_Library/Core/src/usbpd_trace.c

# Startup assembly (any .s in startup folder)
SRC_S := $(call rwildcard,Core/Startup/,*.s)

# Object file list (mirror directory structure under build)
OBJ_C := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRC_C))
OBJ_S := $(patsubst %.s,$(BUILD_DIR)/%.o,$(SRC_S))
OBJS := $(OBJ_C) $(OBJ_S)

# Default target
all: $(TARGET).elf $(TARGET).hex $(TARGET).bin size

# Create build directory tree
$(BUILD_DIR)/%/:
	-@$(MKDIR_P) "$(patsubst %/,%,$@)"

# Prevent make from trying to delete created directories on errors
.PRECIOUS: $(BUILD_DIR)/%/

# Pattern rules
$(BUILD_DIR)/%.o: %.c | $(dir $(BUILD_DIR)/%/)
	@echo CC $<
	@$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/%.o: %.s | $(dir $(BUILD_DIR)/%/)
	@echo AS $<
	@$(AS) -c $(ASFLAGS) -o $@ $<

# Link

$(TARGET).elf: $(OBJS)
	@echo LD $@
	-@$(RM_F) "$@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS) -Wl,--start-group $(LDLIBS) -lc -lm -lnosys -Wl,--end-group

# Post-build
$(TARGET).hex: $(TARGET).elf
	-@$(RM_F) "$@"
	@$(OBJCOPY) -O ihex $< $@

$(TARGET).bin: $(TARGET).elf
	-@$(RM_F) "$@"
	@$(OBJCOPY) -O binary $< $@

size: $(TARGET).elf
	@$(SIZE) --format=berkeley $<

# Clean
clean:
	-$(RM_RF) $(BUILD_DIR)

# Flashing
# Choose via FLASH_TOOL: stlink (STM32_Programmer_CLI) or openocd
FLASH_TOOL ?= stlink
FLASH_ADDR ?= 0x08000000
STLINK_SN ?= 002F00343234510836303532

flash: $(TARGET).elf $(TARGET).hex
ifeq ($(FLASH_TOOL),stlink)
	@echo Flashing with STM32_Programmer_CLI
	@STM32_Programmer_CLI -c port=SWD $(if $(STLINK_SN),sn=$(STLINK_SN),) -w $(TARGET).hex -v -rst
else ifeq ($(FLASH_TOOL),openocd)
	@echo Flashing with OpenOCD
	@openocd -f interface/stlink.cfg -f target/stm32g4x.cfg -c "program $(TARGET).elf verify reset exit"
else
	@echo Unknown FLASH_TOOL '$(FLASH_TOOL)'. Use 'stlink' or 'openocd'.
	@false
endif

.PHONY: all clean flash size
