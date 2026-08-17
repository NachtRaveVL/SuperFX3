cmake_minimum_required(VERSION 3.13...3.31)

# The firmware is physically tied to boards/snes_fx3.h. Pico SDK 2.x must know
# both the custom board search path and selected board before SDK import.
list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards")
set(PICO_BOARD snes_fx3 CACHE STRING "Pico board")

if(NOT PICO_BOARD STREQUAL "snes_fx3")
    message(FATAL_ERROR "SuperFX3 firmware requires PICO_BOARD=snes_fx3; found ${PICO_BOARD}.")
endif()

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake")
elseif(DEFINED ENV{PICO_SDK_PATH} AND EXISTS "$ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    include("$ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake")
else()
    message(FATAL_ERROR "Set PICO_SDK_PATH or copy pico_sdk_import.cmake into the project root.")
endif()

project(superfx3 C CXX ASM)

set(SNES_PARALLEL_ROM_COUNT 1 CACHE STRING "Populated parallel ROM devices (1 or 2)")
set_property(CACHE SNES_PARALLEL_ROM_COUNT PROPERTY STRINGS 1 2)
if(NOT SNES_PARALLEL_ROM_COUNT STREQUAL "1" AND NOT SNES_PARALLEL_ROM_COUNT STREQUAL "2")
    message(FATAL_ERROR "SNES_PARALLEL_ROM_COUNT must be 1 or 2.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

if(PICO_SDK_VERSION_STRING VERSION_LESS "2.3.0")
    message(FATAL_ERROR "SuperFX3 requires Pico SDK 2.3.0 or newer; found ${PICO_SDK_VERSION_STRING}.")
endif()

add_executable(superfx3
    src/main.cpp

    src/fx/fx_core.cpp
    src/fx/fx_decode.cpp
    src/fx/fx_ops_control.cpp
    src/fx/fx_ops_alu.cpp
    src/fx/fx_ops_data.cpp
    src/fx/fx_memory.cpp
    src/fx/fx_registers.cpp
    src/fx/fx_graphics.cpp
    src/fx/fx3_graphics.cpp
    src/fx/fx3_commands.cpp

    src/platform/rp2350/fx_backend.cpp
    src/platform/rp2350/fx_sync.cpp
    src/platform/rp2350/snes_bus.cpp
    src/platform/rp2350/snes_pio.cpp
)

# Pioasm generates snes_bus.pio.h and adds its generated include directory to this target.
pico_generate_pio_header(
    superfx3
    ${CMAKE_CURRENT_LIST_DIR}/src/platform/rp2350/snes_bus.pio
)

target_include_directories(superfx3 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/fx
    ${CMAKE_CURRENT_LIST_DIR}/src/platform/rp2350
)

target_compile_definitions(superfx3 PRIVATE
    SNES_PARALLEL_ROM_COUNT=${SNES_PARALLEL_ROM_COUNT}
    $<$<CONFIG:Debug>:PARAM_ASSERTIONS_ENABLED_HARDWARE_PIO=1>
)

target_compile_options(superfx3 PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>
)

target_link_libraries(superfx3 PRIVATE
    hardware_clocks
    hardware_dma
    hardware_gpio
    hardware_irq
    hardware_pio
    pico_multicore
    pico_sync
    pico_stdlib
)

pico_enable_stdio_uart(superfx3 0)
pico_enable_stdio_usb(superfx3 0)
pico_add_extra_outputs(superfx3)
