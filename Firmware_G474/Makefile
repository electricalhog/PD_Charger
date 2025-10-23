# Minimal Makefile placeholder; expects HAL and USBPD paths provided via env

TARGET      := g474_usbpd_source
BUILD_DIR   := build
CC          := arm-none-eabi-gcc
CXX         := arm-none-eabi-g++
AS          := arm-none-eabi-as
AR          := arm-none-eabi-ar
LD          := arm-none-eabi-g++
OBJCOPY     := arm-none-eabi-objcopy
OBJDUMP     := arm-none-eabi-objdump

CFLAGS := -mcpu=cortex-m4 -mthumb -O2 -ffunction-sections -fdata-sections -Wall -Wextra \
          -IInc -IInc/app -IInc/periphery -IInc/pd -IInc/board -IInc/dsp \
          -I$(HAL_G4_PATH)/Inc

LDFLAGS := -Wl,--gc-sections -TSTM32G474RETX_FLASH.ld

SRCS_C  := \
    Src/main.c \
    Src/pd/UsbPdPort_STUSB1602.c

SRCS_CPP := \
    Src/app/Application.cpp \
    Src/periphery/Hrtim.cpp \
    Src/periphery/AdcIf.cpp \
    Src/board/Board.cpp \
    Src/pd/UsbPdGlue.cpp

OBJS := $(SRCS_C:.c=.o) $(SRCS_CPP:.cpp=.o)

all: $(BUILD_DIR)/$(TARGET).elf

$(BUILD_DIR)/$(TARGET).elf: $(OBJS) | $(BUILD_DIR)
	$(LD) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)
	$(OBJCOPY) -O binary $@ $(BUILD_DIR)/$(TARGET).bin

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(OBJS)

.PHONY: all clean
