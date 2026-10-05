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

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SUPERFX3_BOARD_REVISION "A" CACHE STRING "SuperFX3 PCB revision (A or B)")
set_property(CACHE SUPERFX3_BOARD_REVISION PROPERTY STRINGS A B)
if(SUPERFX3_BOARD_REVISION STREQUAL "A")
    set(SUPERFX3_BOARD_REVISION_NUMBER 1)
    set(SUPERFX3_AUDIO_SD_ENABLED 0)
elseif(SUPERFX3_BOARD_REVISION STREQUAL "B")
    set(SUPERFX3_BOARD_REVISION_NUMBER 2)
    set(SUPERFX3_AUDIO_SD_ENABLED 1)
else()
    message(FATAL_ERROR "SUPERFX3_BOARD_REVISION must be A or B")
endif()

pico_sdk_init()

if(PICO_SDK_VERSION_STRING VERSION_LESS "2.3.0")
    message(FATAL_ERROR "SuperFX3 requires Pico SDK 2.3.0 or newer; found ${PICO_SDK_VERSION_STRING}.")
endif()

add_executable(superfx3
    src/main.cpp

    src/audio/fx3_audio_stream.cpp
    src/video/fx3_video_stream.cpp

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

    src/storage/fx3_save_journal.cpp
    src/storage/parallel_rom_programmer.cpp
    src/storage/snes_rom_layout.cpp
    src/storage/snes_rom_installer.cpp
    src/storage/usb_rom_volume.cpp

    src/platform/rp2350/fx_backend.cpp
    src/platform/rp2350/fx3_audio_sd.cpp
    src/platform/rp2350/fx3_video_sd.cpp
    src/platform/rp2350/sd_audio_source.cpp
    src/platform/rp2350/fx_sync.cpp
    src/platform/rp2350/snes_bus.cpp
    src/platform/rp2350/snes_pio.cpp
    src/platform/rp2350/parallel_rom_gpio.cpp
    src/platform/rp2350/qspi_save.cpp
    src/platform/rp2350/qspi_bus.cpp
    src/platform/rp2350/qspi_sd.cpp
    src/platform/rp2350/qspi_rom.cpp

    src/usb/usb_descriptors.cpp
    src/usb/usb_rom_loader.cpp
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
    ${CMAKE_CURRENT_LIST_DIR}/src/usb
)

target_compile_definitions(superfx3 PRIVATE
    SUPERFX3_BOARD_REVISION=${SUPERFX3_BOARD_REVISION_NUMBER}
    SUPERFX3_AUDIO_SD=${SUPERFX3_AUDIO_SD_ENABLED}
    $<$<AND:$<CONFIG:Debug>,$<STREQUAL:${SUPERFX3_BOARD_REVISION},A>>:PARAM_ASSERTIONS_ENABLED_HARDWARE_PIO=1>
)

if(SUPERFX3_BOARD_REVISION STREQUAL "B")
    target_compile_definitions(superfx3 PRIVATE
        PARAM_ASSERTIONS_ENABLED_HARDWARE_PIO=0
        PICO_NO_RAM_VECTOR_TABLE=0
    )
    # Live SD roots must not acquire compiler-generated flash jump/lookup tables.
    set_source_files_properties(
        src/audio/fx3_audio_stream.cpp
        src/video/fx3_video_stream.cpp
        src/fx/fx_registers.cpp
        src/platform/rp2350/fx_sync.cpp
        src/platform/rp2350/fx_backend.cpp
        src/platform/rp2350/snes_pio.cpp
        src/platform/rp2350/snes_bus.cpp
        src/platform/rp2350/qspi_bus.cpp
        src/platform/rp2350/qspi_sd.cpp
        PROPERTIES COMPILE_OPTIONS "-O2;-fno-jump-tables;-fno-tree-switch-conversion;-fno-lto"
    )
endif()

target_compile_options(superfx3 PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>
)

target_link_libraries(superfx3 PRIVATE
    hardware_clocks
    hardware_dma
    hardware_gpio
    hardware_flash
    hardware_irq
    hardware_pio
    pico_multicore
    pico_flash
    pico_sync
    pico_stdlib
    tinyusb_device
    tinyusb_board
)

pico_enable_stdio_uart(superfx3 0)
pico_enable_stdio_usb(superfx3 0)
pico_add_extra_outputs(superfx3)
# The diagnostic ROM is intentionally a separate target. Firmware-only builds do
# not need cc65, while `fx3_testrom` gives us the 65816 supervisor and GSU-visible ROM.
find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)

if(SUPERFX3_BOARD_REVISION STREQUAL "B")
    add_custom_command(TARGET superfx3 POST_BUILD
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_LIST_DIR}/src/tools/check_qspi_sd_sram.py
                $<TARGET_FILE:superfx3> --objdump ${CMAKE_OBJDUMP}
        VERBATIM
    )
endif()

add_custom_command(
    TARGET superfx3
    POST_BUILD
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/src/tools/check_firmware_size.py
            ${CMAKE_CURRENT_BINARY_DIR}/superfx3.bin
            $<TARGET_FILE:superfx3>
    COMMENT "Checking SuperFX3 firmware size"
    VERBATIM
)
add_custom_target(fx3_testrom
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/testrom/build.py
            --build-dir ${CMAKE_BINARY_DIR}/testrom
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Building FX3 diagnostic ROM"
    VERBATIM
)

add_custom_target(fx3_testrom_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/testrom/build.py
            --check --build-dir ${CMAKE_BINARY_DIR}/testrom-check
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Checking FX3 diagnostic ROM sources"
    VERBATIM
)
