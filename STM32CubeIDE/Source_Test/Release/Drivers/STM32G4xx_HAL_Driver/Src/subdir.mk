################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (12.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_cortex.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma_ex.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_exti.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ex.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ramfunc.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_gpio.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c_ex.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr_ex.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc_ex.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim_ex.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart_ex.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart_ex.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_adc.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_dma.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_exti.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_gpio.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_lpuart.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_rcc.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_tim.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_ucpd.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_usart.c \
../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_utils.c 

OBJS += \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_cortex.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma_ex.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_exti.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ex.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ramfunc.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_gpio.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c_ex.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr_ex.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc_ex.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim_ex.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart_ex.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart_ex.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_adc.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_dma.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_exti.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_gpio.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_lpuart.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_rcc.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_tim.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_ucpd.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_usart.o \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_utils.o 

C_DEPS += \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_cortex.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma_ex.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_exti.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ex.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ramfunc.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_gpio.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c_ex.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr_ex.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc_ex.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim_ex.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart_ex.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart_ex.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_adc.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_dma.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_exti.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_gpio.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_lpuart.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_rcc.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_tim.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_ucpd.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_usart.d \
./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_utils.d 


# Each subdirectory must supply rules for building sources it contributes
Drivers/STM32G4xx_HAL_Driver/Src/%.o Drivers/STM32G4xx_HAL_Driver/Src/%.su Drivers/STM32G4xx_HAL_Driver/Src/%.cyclo: ../Drivers/STM32G4xx_HAL_Driver/Src/%.c Drivers/STM32G4xx_HAL_Driver/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -DUSE_NUCLEO_64 -DUSE_HAL_DRIVER -DSTM32G431xx -DUSE_FULL_LL_DRIVER -DUSBPD_PORT_COUNT=1 -D_RTOS -D_SRC -D_TRACE -D_GUI_INTERFACE -DUSBPDCORE_LIB_PD3_FULL -DTCPP0203_SUPPORT -c -I../Core/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc/Legacy -I../Drivers/BSP/STM32G4xx_Nucleo -I../Drivers/CMSIS/Device/ST/STM32G4xx/Include -I../Drivers/CMSIS/Include -I../TCPP/App -I../TCPP/Target -I../TCPP -I../USBPD/App -I../USBPD -I../Utilities/GUI_INTERFACE -I../Utilities/TRACER_EMB -I../Middlewares/Third_Party/FreeRTOS/Source/include -I../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS -I../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F -I../Middlewares/ST/STM32_USBPD_Library/Core/inc -I../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/inc -I../Drivers/BSP/X-NUCLEO-SRC1M1 -I../Drivers/BSP/Components/tcpp0203 -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Drivers-2f-STM32G4xx_HAL_Driver-2f-Src

clean-Drivers-2f-STM32G4xx_HAL_Driver-2f-Src:
	-$(RM) ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_cortex.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_cortex.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_cortex.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_cortex.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma_ex.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma_ex.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma_ex.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma_ex.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_exti.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_exti.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_exti.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_exti.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ex.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ex.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ex.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ex.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ramfunc.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ramfunc.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ramfunc.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ramfunc.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_gpio.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_gpio.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_gpio.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_gpio.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c_ex.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c_ex.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c_ex.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c_ex.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr_ex.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr_ex.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr_ex.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr_ex.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc_ex.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc_ex.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc_ex.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc_ex.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim_ex.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim_ex.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim_ex.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim_ex.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart_ex.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart_ex.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart_ex.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart_ex.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart_ex.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart_ex.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart_ex.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_usart_ex.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_adc.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_adc.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_adc.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_adc.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_dma.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_dma.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_dma.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_dma.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_exti.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_exti.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_exti.o
	-$(RM) ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_exti.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_gpio.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_gpio.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_gpio.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_gpio.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_lpuart.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_lpuart.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_lpuart.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_lpuart.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_rcc.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_rcc.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_rcc.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_rcc.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_tim.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_tim.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_tim.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_tim.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_ucpd.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_ucpd.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_ucpd.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_ucpd.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_usart.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_usart.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_usart.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_usart.su ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_utils.cyclo ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_utils.d ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_utils.o ./Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_utils.su

.PHONY: clean-Drivers-2f-STM32G4xx_HAL_Driver-2f-Src

