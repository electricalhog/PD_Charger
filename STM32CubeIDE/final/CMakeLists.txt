cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME final)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    Core/Src/pd_policy.c
    Core/Src/pd_vbus.c
    Core/Src/buck_ccm.c
)

# USB-PD VBUS path.  ON: the Type-C VBUS is this board's regulator output
# behind the output switch (Core/Src/pd_vbus.c); the X-NUCLEO-SRC1M1 BSP is
# left out of the link because it drives PC8 (OUTPUT_EN here, TCPP03 ENABLE
# on that shield) and reprograms ADC1/DMA, and the regulator only runs for a
# negotiated contract (PD_OWNS_VBUS in pd_interface.h).  OFF: the previous
# bench build (SRC1M1 BSP, default task starts the regulator at
# DEFAULT_TASK_TEST_VOLTAGE_MV, mailbox start/set-voltage allowed); use it
# only with nothing plugged into the Type-C receptacle.
option(PD_VBUS_PATH_CHARGER "USB-PD VBUS through the regulator and output switch" ON)
# VCONN switches on GPIOs (pins in Core/Inc/pd_bench_config.h).  Needed for
# EPR entry: the source must power the cable e-marker to read it over SOP'.
option(PD_VCONN "Source VCONN through GPIO-driven switches" OFF)
# Bench dry run: the PD stack is told VBUS follows the contract, but the
# regulator never starts (Core/Src/pd_vbus.c).  For protocol tests (VCONN,
# SOP' cable discovery, EPR messages) with nothing taking power.
option(PD_BENCH_DRY_RUN "PD protocol without VBUS (regulator never starts)" OFF)
# Bench: be VCONN source at every attach, as if the CAD had seen the cable's
# Ra (ATTEMC).  For a cable whose e-marker shows no Ra at this end.
option(PD_BENCH_FORCE_VCONN "VCONN source at every attach (needs PD_VCONN)" OFF)
# Bench: offer 15 V (the buck load's gate drive needs 12-15 V), and 20 V as
# well.  At the 80 V/s up-slew both are reachable straight from 5 V
# (Core/Inc/pd_bench_config.h checks the transition budgets).
option(PD_BENCH_15V "Offer the 15 V PDO" OFF)
option(PD_BENCH_20V "Offer the 20 V PDO (needs PD_BENCH_15V)" OFF)
# Bench: a non-standard 24 V fixed PDO, in the buck-boost band at 24 V in.
option(PD_BENCH_24V "Offer a 24 V fixed PDO (bench only, not in the spec)" OFF)
# Bench: one SPR PPS APDO (5 V to 11/16/21 V, Current Limit in CCM and DCM),
# see PD_SRC_APDO_PPS_ENABLE in Core/Inc/pd_bench_config.h and plans/pps.md.
option(PD_BENCH_PPS "Offer an SPR PPS APDO" OFF)
# Bench: offer a 28 V fixed EPR PDO (position 8).  Needs VCONN for the
# e-marker check; see PD_EPR_PDO_28V_ENABLE in Core/Inc/pd_bench_config.h.
option(PD_BENCH_EPR_28V "Offer the 28 V EPR PDO (bench, needs PD_VCONN)" OFF)
# Bench: 36 and 48 V fixed EPR PDOs, each on top of the one below
# (PD_EPR_PDO_36V_ENABLE / _48V_ENABLE in Core/Inc/pd_bench_config.h).
option(PD_BENCH_EPR_36V "Offer the 36 V EPR PDO (bench, needs PD_BENCH_EPR_28V)" OFF)
option(PD_BENCH_EPR_48V "Offer the 48 V EPR PDO (bench, needs PD_BENCH_EPR_36V)" OFF)

# Battery-load build (branch claude/lipo-load, plans/lipo-load.md): a LiPo on
# VBUS as a power sink, and no USB at all.  Not an option on this branch: the
# USB-PD, UCPD, TCPP and tracer sources are left out (Core/Src/battery_no_usb.c
# stubs what the generated code still calls), and every PD option must be off.
# Use the Battery preset (build/Battery), so the PD branches' build/Debug cache
# is never reconfigured from here.
set(BATTERY_LOAD ON)
foreach(opt PD_VBUS_PATH_CHARGER PD_VCONN PD_BENCH_DRY_RUN PD_BENCH_FORCE_VCONN PD_BENCH_15V PD_BENCH_20V
        PD_BENCH_24V PD_BENCH_PPS PD_BENCH_EPR_28V PD_BENCH_EPR_36V PD_BENCH_EPR_48V)
    if(${opt})
        message(FATAL_ERROR "${opt} is ON: this branch is the battery-load build with no USB. "
                            "Configure with: cmake --preset Battery")
    endif()
endforeach()
target_compile_definitions(stm32cubemx INTERFACE BATTERY_LOAD=1)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE Core/Src/battery_no_usb.c)
set(BATTERY_NO_USB_SRCS
    TCPP/App/app_tcpp.c
    USBPD/App/usbpd.c USBPD/App/usbpd_pwr_if.c USBPD/App/usbpd_dpm_core.c
    USBPD/Target/usbpd_dpm_user.c USBPD/Target/usbpd_pwr_user.c USBPD/Target/usbpd_vdm_user.c
    Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_usbpd_pwr.c
    Utilities/GUI_INTERFACE/data_struct_tlv.c Utilities/GUI_INTERFACE/bsp_gui.c Utilities/GUI_INTERFACE/gui_api.c
    Utilities/TRACER_EMB/tracer_emb.c Utilities/TRACER_EMB/tracer_emb_hw.c
    Middlewares/ST/STM32_USBPD_Library/Core/src/usbpd_trace.c
    Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_cad_hw_if.c
    Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_hw.c
    Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_hw_if_it.c
    Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_phy.c
    Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_phy_hw_if.c
    Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_pwr_hw_if.c
    Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/src/usbpd_timersserver.c
    Core/Src/pd_policy.c Core/Src/pd_vbus.c)
list(TRANSFORM BATTERY_NO_USB_SRCS PREPEND ${CMAKE_SOURCE_DIR}/)
set_source_files_properties(${BATTERY_NO_USB_SRCS}
    DIRECTORY ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/cmake/stm32cubemx
    PROPERTIES HEADER_FILE_ONLY ON)

if(PD_VBUS_PATH_CHARGER)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PD_VBUS_PATH_CHARGER=1)
    # The generated lists add it twice (application sources and the
    # STM32_Drivers object library), so drop it in both directory scopes.
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/Drivers/BSP/X-NUCLEO-SRC1M1/src1m1_usbpd_pwr.c
        DIRECTORY ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/cmake/stm32cubemx
        PROPERTIES HEADER_FILE_ONLY ON)
endif()
if(PD_BENCH_DRY_RUN)
    if(NOT PD_VBUS_PATH_CHARGER)
        message(FATAL_ERROR "PD_BENCH_DRY_RUN needs PD_VBUS_PATH_CHARGER")
    endif()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PD_BENCH_DRY_RUN=1)
endif()
if(PD_VCONN)
    if(NOT PD_VBUS_PATH_CHARGER)
        message(FATAL_ERROR "PD_VCONN needs PD_VBUS_PATH_CHARGER")
    endif()
    # _VCONN_SUPPORT also gates the VCONN calls in the USBPD device driver
    # (usbpd_pwr_hw_if.c), which builds from the stm32cubemx interface.
    target_compile_definitions(stm32cubemx INTERFACE _VCONN_SUPPORT PD_VCONN_ENABLE=1)
endif()
if(PD_BENCH_FORCE_VCONN)
    if(NOT PD_VCONN)
        message(FATAL_ERROR "PD_BENCH_FORCE_VCONN needs PD_VCONN")
    endif()
    target_compile_definitions(stm32cubemx INTERFACE PD_BENCH_FORCE_VCONN=1)
endif()
if(PD_BENCH_15V)
    if(NOT PD_VBUS_PATH_CHARGER)
        message(FATAL_ERROR "PD_BENCH_15V needs PD_VBUS_PATH_CHARGER")
    endif()
    target_compile_definitions(stm32cubemx INTERFACE PD_BENCH_15V=1)
endif()
if(PD_BENCH_20V)
    if(NOT PD_BENCH_15V)
        message(FATAL_ERROR "PD_BENCH_20V needs PD_BENCH_15V")
    endif()
    target_compile_definitions(stm32cubemx INTERFACE PD_BENCH_20V=1)
endif()
if(PD_BENCH_24V)
    target_compile_definitions(stm32cubemx INTERFACE PD_BENCH_24V=1)
endif()
if(PD_BENCH_PPS)
    if(NOT PD_VBUS_PATH_CHARGER)
        message(FATAL_ERROR "PD_BENCH_PPS needs PD_VBUS_PATH_CHARGER")
    endif()
    target_compile_definitions(stm32cubemx INTERFACE PD_BENCH_PPS=1)
endif()
if(PD_BENCH_EPR_28V)
    if(NOT PD_VCONN)
        message(FATAL_ERROR "PD_BENCH_EPR_28V needs PD_VCONN (EPR entry checks the cable e-marker)")
    endif()
    target_compile_definitions(stm32cubemx INTERFACE PD_BENCH_EPR_28V=1)
endif()
if(PD_BENCH_EPR_36V)
    if(NOT PD_BENCH_EPR_28V)
        message(FATAL_ERROR "PD_BENCH_EPR_36V needs PD_BENCH_EPR_28V")
    endif()
    target_compile_definitions(stm32cubemx INTERFACE PD_BENCH_EPR_36V=1)
endif()
if(PD_BENCH_EPR_48V)
    if(NOT PD_BENCH_EPR_36V)
        message(FATAL_ERROR "PD_BENCH_EPR_48V needs PD_BENCH_EPR_36V")
    endif()
    target_compile_definitions(stm32cubemx INTERFACE PD_BENCH_EPR_48V=1)
endif()

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Control-loop sources at -O2 in every build type (they keep -g3 in Debug).
# At -O0 the PID ISR took 67 us of its 50 us period in boost and the Timer A
# period ISR 2 us of 5 us, starving every lower-priority interrupt and task
# (bench, 2026-09-28 evening, DWT counts in regulator_cyc_*).
set_source_files_properties(
    Core/Src/regulator.c Core/Src/adc_monitor.c Core/Src/pid_controller.c Core/Src/buck_ccm.c
    Core/Src/slope_comp.c Core/Src/debug_log.c Core/Src/stm32g4xx_it.c
    DIRECTORY ${CMAKE_SOURCE_DIR}
    PROPERTIES COMPILE_OPTIONS "-O2")

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
