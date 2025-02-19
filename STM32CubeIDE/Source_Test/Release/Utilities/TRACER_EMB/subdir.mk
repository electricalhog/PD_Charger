################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (12.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Utilities/TRACER_EMB/tracer_emb.c \
../Utilities/TRACER_EMB/tracer_emb_hw.c 

OBJS += \
./Utilities/TRACER_EMB/tracer_emb.o \
./Utilities/TRACER_EMB/tracer_emb_hw.o 

C_DEPS += \
./Utilities/TRACER_EMB/tracer_emb.d \
./Utilities/TRACER_EMB/tracer_emb_hw.d 


# Each subdirectory must supply rules for building sources it contributes
Utilities/TRACER_EMB/%.o Utilities/TRACER_EMB/%.su Utilities/TRACER_EMB/%.cyclo: ../Utilities/TRACER_EMB/%.c Utilities/TRACER_EMB/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -DUSE_NUCLEO_64 -DUSE_HAL_DRIVER -DSTM32G431xx -DUSE_FULL_LL_DRIVER -DUSBPD_PORT_COUNT=1 -D_RTOS -D_SRC -D_TRACE -D_GUI_INTERFACE -DUSBPDCORE_LIB_PD3_FULL -DTCPP0203_SUPPORT -c -I../Core/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc/Legacy -I../Drivers/BSP/STM32G4xx_Nucleo -I../Drivers/CMSIS/Device/ST/STM32G4xx/Include -I../Drivers/CMSIS/Include -I../TCPP/App -I../TCPP/Target -I../TCPP -I../USBPD/App -I../USBPD -I../Utilities/GUI_INTERFACE -I../Utilities/TRACER_EMB -I../Middlewares/Third_Party/FreeRTOS/Source/include -I../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS -I../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F -I../Middlewares/ST/STM32_USBPD_Library/Core/inc -I../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/inc -I../Drivers/BSP/X-NUCLEO-SRC1M1 -I../Drivers/BSP/Components/tcpp0203 -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Utilities-2f-TRACER_EMB

clean-Utilities-2f-TRACER_EMB:
	-$(RM) ./Utilities/TRACER_EMB/tracer_emb.cyclo ./Utilities/TRACER_EMB/tracer_emb.d ./Utilities/TRACER_EMB/tracer_emb.o ./Utilities/TRACER_EMB/tracer_emb.su ./Utilities/TRACER_EMB/tracer_emb_hw.cyclo ./Utilities/TRACER_EMB/tracer_emb_hw.d ./Utilities/TRACER_EMB/tracer_emb_hw.o ./Utilities/TRACER_EMB/tracer_emb_hw.su

.PHONY: clean-Utilities-2f-TRACER_EMB

