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
    Core/Src/regulator.c Core/Src/adc_monitor.c Core/Src/pid_controller.c
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
