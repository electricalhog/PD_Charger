################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (12.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_bus.c \
../Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_usbpd_pwr.c 

OBJS += \
./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_bus.o \
./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_usbpd_pwr.o 

C_DEPS += \
./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_bus.d \
./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_usbpd_pwr.d 


# Each subdirectory must supply rules for building sources it contributes
Drivers/BSP/X-NUCLEO-SRC1M1/%.o Drivers/BSP/X-NUCLEO-SRC1M1/%.su Drivers/BSP/X-NUCLEO-SRC1M1/%.cyclo: ../Drivers/BSP/X-NUCLEO-SRC1M1/%.c Drivers/BSP/X-NUCLEO-SRC1M1/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -DUSE_NUCLEO_64 -DUSE_HAL_DRIVER -DSTM32G431xx -DUSE_FULL_LL_DRIVER -DUSBPD_PORT_COUNT=1 -D_RTOS -D_SRC -D_TRACE -D_GUI_INTERFACE -DUSBPDCORE_LIB_PD3_FULL -DTCPP0203_SUPPORT -c -I../Core/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc/Legacy -I../Drivers/BSP/STM32G4xx_Nucleo -I../Drivers/CMSIS/Device/ST/STM32G4xx/Include -I../Drivers/CMSIS/Include -I../TCPP/App -I../TCPP/Target -I../TCPP -I../USBPD/App -I../USBPD -I../Utilities/GUI_INTERFACE -I../Utilities/TRACER_EMB -I../Middlewares/Third_Party/FreeRTOS/Source/include -I../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS -I../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F -I../Middlewares/ST/STM32_USBPD_Library/Core/inc -I../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/inc -I../Drivers/BSP/X-NUCLEO-SRC1M1 -I../Drivers/BSP/Components/tcpp0203 -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Drivers-2f-BSP-2f-X-2d-NUCLEO-2d-SRC1M1

clean-Drivers-2f-BSP-2f-X-2d-NUCLEO-2d-SRC1M1:
	-$(RM) ./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_bus.cyclo ./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_bus.d ./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_bus.o ./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_bus.su ./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_usbpd_pwr.cyclo ./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_usbpd_pwr.d ./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_usbpd_pwr.o ./Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_usbpd_pwr.su

.PHONY: clean-Drivers-2f-BSP-2f-X-2d-NUCLEO-2d-SRC1M1

