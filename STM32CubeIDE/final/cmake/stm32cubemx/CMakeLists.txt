cmake_minimum_required(VERSION 3.22)
# Enable CMake support for ASM and C languages
enable_language(C ASM)
# STM32CubeMX generated symbols (macros)
set(MX_Defines_Syms 
	USE_FULL_LL_DRIVER 
	USBPD_PORT_COUNT=1 
	_RTOS 
	_SRC 
	_TRACE 
	_GUI_INTERFACE 
	USBPDCORE_LIB_PD3_FULL 
	TCPP0203_SUPPORT 
	USE_HAL_DRIVER 
	STM32G474xx
    $<$<CONFIG:Debug>:DEBUG>
)

# STM32CubeMX generated include paths
set(MX_Include_Dirs
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../TCPP/App
    ${CMAKE_CURRENT_SOURCE_DIR}/../../TCPP/Target
    ${CMAKE_CURRENT_SOURCE_DIR}/../../TCPP
    ${CMAKE_CURRENT_SOURCE_DIR}/../../USBPD/App
    ${CMAKE_CURRENT_SOURCE_DIR}/../../USBPD/Target
    ${CMAKE_CURRENT_SOURCE_DIR}/../../USBPD
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Inc/Legacy
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Utilities/GUI_INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Utilities/TRACER_EMB
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/ST/STM32_USBPD_Library/Core/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/CMSIS/Device/ST/STM32G4xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/CMSIS/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/BSP/X-NUCLEO-SRC1M1
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/BSP/Components/tcpp0203
)

# STM32CubeMX generated application sources
set(MX_Application_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../TCPP/App/app_tcpp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../USBPD/App/usbpd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../USBPD/App/usbpd_pwr_if.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../USBPD/App/usbpd_dpm_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../USBPD/Target/usbpd_dpm_user.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../USBPD/Target/usbpd_pwr_user.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../USBPD/Target/usbpd_vdm_user.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/app_freertos.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/stm32g4xx_it.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/stm32g4xx_hal_msp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/stm32g4xx_hal_timebase_tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/sysmem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/syscalls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../startup_stm32g474xx.s
)

# STM32 HAL/LL Drivers
set(STM32_Drivers_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Core/Src/system_stm32g4xx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_exti.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_tim_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_adc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_rcc_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_flash_ramfunc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_exti.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dma_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_pwr_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_cortex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_rcc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_pwr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_comp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dac.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_dac_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_hrtim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_i2c_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_uart_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_usart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_lpuart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_ucpd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_dma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_ll_tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/BSP/Components/tcpp0203/tcpp0203.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/BSP/Components/tcpp0203/tcpp0203_reg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_bus.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_usbpd_pwr.c
)

# Drivers Midllewares


set(Utilities_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Utilities/GUI_INTERFACE/data_struct_tlv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Utilities/GUI_INTERFACE/bsp_gui.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Utilities/GUI_INTERFACE/gui_api.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Utilities/TRACER_EMB/tracer_emb.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Utilities/TRACER_EMB/tracer_emb_hw.c
)
set(FreeRTOS_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/croutine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/event_groups.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/queue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/stream_buffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/tasks.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/timers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS/cmsis_os.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F/port.c
)
set(USBPD_Src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/ST/STM32_USBPD_Library/Core/src/usbpd_trace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_cad_hw_if.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_hw_if_it.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_phy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_phy_hw_if.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_pwr_hw_if.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_timersserver.c
)

# Link directories setup
set(MX_LINK_DIRS

    ${CMAKE_CURRENT_SOURCE_DIR}/../../Middlewares/ST/STM32_USBPD_Library/Core/lib
)
# Project static libraries
set(MX_LINK_LIBS  
	:USBPDCORE_PD3_FULL_CM4_wc32.a
    STM32_Drivers
    ${TOOLCHAIN_LINK_LIBRARIES}
    Utilities	FreeRTOS	USBPD	
)
# Interface library for includes and symbols
add_library(stm32cubemx INTERFACE)
target_include_directories(stm32cubemx INTERFACE ${MX_Include_Dirs})
target_compile_definitions(stm32cubemx INTERFACE ${MX_Defines_Syms})

# Create STM32_Drivers static library
add_library(STM32_Drivers OBJECT)
target_sources(STM32_Drivers PRIVATE ${STM32_Drivers_Src})
target_link_libraries(STM32_Drivers PUBLIC stm32cubemx)


# Create Utilities static library
add_library(Utilities OBJECT)
target_sources(Utilities PRIVATE ${Utilities_Src})
target_link_libraries(Utilities PUBLIC stm32cubemx)

# Create FreeRTOS static library
add_library(FreeRTOS OBJECT)
target_sources(FreeRTOS PRIVATE ${FreeRTOS_Src})
target_link_libraries(FreeRTOS PUBLIC stm32cubemx)

# Create USBPD static library
add_library(USBPD OBJECT)
target_sources(USBPD PRIVATE ${USBPD_Src})
target_link_libraries(USBPD PUBLIC stm32cubemx)

# Add STM32CubeMX generated application sources to the project
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${MX_Application_Src})

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE ${MX_LINK_DIRS})

# Add libraries to the project
target_link_libraries(${CMAKE_PROJECT_NAME} ${MX_LINK_LIBS})

# Add the map file to the list of files to be removed with 'clean' target
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES ADDITIONAL_CLEAN_FILES ${CMAKE_PROJECT_NAME}.map)

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()
