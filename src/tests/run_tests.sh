#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/tests"
CXX="${CXX:-g++}"

cd "$ROOT"
rm -rf "$BUILD"
mkdir -p "$BUILD"

COMMON_FLAGS=(
    -std=c++17
    -O2
    -Wall -Wextra -Wpedantic -Werror
    -Wconversion -Wsign-conversion
)
TEST_FLAGS=(-DSUPERFX3_TEST)
CORE_INCLUDES=(-Itests/sdk_stubs -Itests/stubs -I. -Ifx)
PICO_INCLUDES=(-Itests/sdk_stubs -I. -Ifx -Iplatform/rp2350 -Iusb)

CORE_SOURCES=(
    fx/fx_core.cpp
    fx/fx_decode.cpp
    fx/fx_ops_control.cpp
    fx/fx_ops_alu.cpp
    fx/fx_ops_data.cpp
    fx/fx_memory.cpp
    fx/fx_registers.cpp
    fx/fx_graphics.cpp
    fx/fx3_graphics.cpp
    fx/fx3_commands.cpp
)

STORAGE_SOURCES=(
    storage/fx3_save_journal.cpp
    storage/parallel_rom_programmer.cpp
    storage/snes_rom_layout.cpp
    storage/snes_rom_installer.cpp
    storage/usb_rom_volume.cpp
)

PRODUCTION_SOURCES=(
    main.cpp
    audio/fx3_audio_stream.cpp
    video/fx3_video_stream.cpp
    "${CORE_SOURCES[@]}"
    "${STORAGE_SOURCES[@]}"
    platform/rp2350/fx_backend.cpp
    platform/rp2350/fx3_audio_sd.cpp
    platform/rp2350/fx3_video_sd.cpp
    platform/rp2350/sd_audio_source.cpp
    platform/rp2350/fx_sync.cpp
    platform/rp2350/snes_bus.cpp
    platform/rp2350/snes_pio.cpp
    platform/rp2350/parallel_rom_gpio.cpp
    platform/rp2350/qspi_save.cpp
    platform/rp2350/qspi_bus.cpp
    platform/rp2350/qspi_sd.cpp
    platform/rp2350/qspi_rom.cpp
    usb/usb_descriptors.cpp
    usb/usb_rom_loader.cpp
    tests/sdk_stubs/flash_end.cpp
)

if [[ -t 1 && -z "${NO_COLOR:-}" && "${TERM:-dumb}" != "dumb" ]]; then
    BOLD=$'\033[1m'
    GREEN=$'\033[32m'
    RED=$'\033[31m'
    RESET=$'\033[0m'
else
    BOLD=""
    GREEN=""
    RED=""
    RESET=""
fi

status() {
    local label="$1"
    local result="$2"
    local color="$GREEN"
    [[ "$result" == "FAIL" ]] && color="$RED"
    printf "    %-34s %b%b%s%b\n" "$label" "$BOLD" "$color" "$result" "$RESET"
}

run_stage() {
    local label="$1"
    shift

    if "$@"; then
        status "$label" "PASS"
    else
        status "$label" "FAIL"
        return 1
    fi
}

build_core_test() {
    local name="$1"
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${CORE_INCLUDES[@]}" \
        "tests/$name.cpp" "${CORE_SOURCES[@]}" -o "$BUILD/$name"
}

build_sync_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        tests/sync_tests.cpp "${CORE_SOURCES[@]}" platform/rp2350/fx_sync.cpp \
        -o "$BUILD/sync_tests"
}


build_bus_integration_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        tests/bus_integration_tests.cpp "${CORE_SOURCES[@]}" platform/rp2350/fx_sync.cpp \
        platform/rp2350/snes_bus.cpp platform/rp2350/snes_pio.cpp \
        audio/fx3_audio_stream.cpp video/fx3_video_stream.cpp storage/snes_rom_layout.cpp \
        -o "$BUILD/bus_integration_tests"
}

build_video_stream_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        -DSUPERFX3_AUDIO_SD=1 tests/video_stream_tests.cpp video/fx3_video_stream.cpp \
        -o "$BUILD/video_stream_tests"
}

build_video_sync_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        -DSUPERFX3_AUDIO_SD=1 tests/video_sync_tests.cpp "${CORE_SOURCES[@]}" \
        platform/rp2350/fx_sync.cpp -o "$BUILD/video_sync_tests"
}

build_audio_stream_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        -DSUPERFX3_AUDIO_SD=1 -DSDK_TEST_THREADED_CRITICAL_SECTIONS -pthread \
        tests/audio_stream_tests.cpp \
        audio/fx3_audio_stream.cpp -o "$BUILD/audio_stream_tests"
}

build_audio_sd_tests() {
    local revision="$1"
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        -DSUPERFX3_BOARD_REVISION="$revision" tests/audio_sd_tests.cpp \
        platform/rp2350/fx3_audio_sd.cpp platform/rp2350/sd_audio_source.cpp \
        -o "$BUILD/audio_sd_tests_rev_$revision"
}

build_qspi_sd_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        -DSUPERFX3_AUDIO_SD=1 -DSUPERFX3_BOARD_REVISION=2 \
        -DSDK_TEST_FLASH_EMULATION -DSDK_TEST_THREADED_CRITICAL_SECTIONS -pthread \
        tests/qspi_sd_tests.cpp "${CORE_SOURCES[@]}" audio/fx3_audio_stream.cpp video/fx3_video_stream.cpp \
        platform/rp2350/fx_sync.cpp platform/rp2350/snes_bus.cpp platform/rp2350/snes_pio.cpp \
        platform/rp2350/qspi_bus.cpp platform/rp2350/qspi_sd.cpp platform/rp2350/qspi_save.cpp \
        storage/fx3_save_journal.cpp -o "$BUILD/qspi_sd_tests"
}

build_sd_audio_source_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        -DSUPERFX3_AUDIO_SD=1 -DSUPERFX3_BOARD_REVISION=2 \
        tests/sd_audio_source_tests.cpp audio/fx3_audio_stream.cpp \
        platform/rp2350/fx3_audio_sd.cpp platform/rp2350/sd_audio_source.cpp \
        -o "$BUILD/sd_audio_source_tests"
}

build_rev_b_bus_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        -DSUPERFX3_BOARD_REVISION=2 tests/bus_integration_tests.cpp "${CORE_SOURCES[@]}" \
        platform/rp2350/fx_sync.cpp platform/rp2350/snes_bus.cpp platform/rp2350/snes_pio.cpp \
        audio/fx3_audio_stream.cpp video/fx3_video_stream.cpp storage/snes_rom_layout.cpp -o "$BUILD/rev_b_bus_tests"
}

build_fx_core_sanity() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        tests/fx_core_sanity.cpp "${CORE_SOURCES[@]}" platform/rp2350/fx_backend.cpp \
        tests/sdk_stubs/flash_end.cpp -o "$BUILD/fx_core_sanity"
}

build_register_backend_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        tests/register_backend_tests.cpp "${CORE_SOURCES[@]}" platform/rp2350/fx_backend.cpp \
        tests/sdk_stubs/flash_end.cpp -o "$BUILD/register_backend_tests"
}

build_save_journal_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${CORE_INCLUDES[@]}" \
        tests/save_journal_tests.cpp "${STORAGE_SOURCES[@]}" \
        -o "$BUILD/save_journal_tests"
}

build_qspi_save_integration_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        -DSDK_TEST_FLASH_EMULATION tests/qspi_save_integration_tests.cpp \
        platform/rp2350/qspi_save.cpp platform/rp2350/qspi_bus.cpp storage/fx3_save_journal.cpp \
        -o "$BUILD/qspi_save_integration_tests"
}

build_save_stop_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        -DSDK_TEST_FLASH_EMULATION tests/save_stop_tests.cpp "${CORE_SOURCES[@]}" \
        platform/rp2350/qspi_save.cpp platform/rp2350/qspi_bus.cpp storage/fx3_save_journal.cpp \
        -o "$BUILD/save_stop_tests"
}

build_parallel_rom_programmer_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${CORE_INCLUDES[@]}" \
        tests/parallel_rom_programmer_tests.cpp storage/parallel_rom_programmer.cpp \
        -o "$BUILD/parallel_rom_programmer_tests"
}

build_snes_rom_layout_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${CORE_INCLUDES[@]}" \
        tests/snes_rom_layout_tests.cpp storage/snes_rom_layout.cpp \
        -o "$BUILD/snes_rom_layout_tests"
}

build_usb_rom_volume_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${CORE_INCLUDES[@]}" \
        tests/usb_rom_volume_tests.cpp storage/usb_rom_volume.cpp \
        -o "$BUILD/usb_rom_volume_tests"
}

build_usb_block_order_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${CORE_INCLUDES[@]}" \
        tests/usb_block_order_tests.cpp storage/usb_rom_volume.cpp \
        -o "$BUILD/usb_block_order_tests"
}

build_snes_rom_installer_tests() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" "${CORE_INCLUDES[@]}" \
        -DSDK_TEST_FLASH_EMULATION -DSDK_TEST_FX_ROM_PROGRAMMING \
        tests/snes_rom_installer_tests.cpp "${STORAGE_SOURCES[@]}" \
        platform/rp2350/qspi_rom.cpp \
        platform/rp2350/qspi_bus.cpp \
        -o "$BUILD/snes_rom_installer_tests"
}

build_production_stub() {
    "$CXX" "${COMMON_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        "${PRODUCTION_SOURCES[@]}" -o "$BUILD/superfx3_stub_link"
}

build_production_stub_rev_b() {
    "$CXX" "${COMMON_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        -DSUPERFX3_AUDIO_SD=1 -DSUPERFX3_BOARD_REVISION=2 \
        "${PRODUCTION_SOURCES[@]}" -o "$BUILD/superfx3_rev_b_stub_link"
}

printf "Compiler: %s\n" "$("$CXX" --version | head -n 1)"

echo
echo "== Python/static tests =="
run_stage "PIO static checks" python3 tests/pio_static_tests.py
run_stage "Live SD SRAM audit checks" python3 tests/qspi_sd_sram_tests.py
run_stage "QSPI image packer" python3 tests/packer_tests.py
run_stage "QSPI/parallel storage separation" python3 tests/storage_separation_tests.py
run_stage "SNES ROM bus image" python3 tests/snes_rom_image_tests.py
run_stage "USB installer preservation plan" python3 tests/usb_installer_plan_tests.py
run_stage "FX3 diagnostic ROM sources" python3 ../testrom/build.py --check --build-dir "$BUILD/testrom-check"

echo
echo "== Portable core tests =="
run_stage "CXX core_tests" build_core_test core_tests
run_stage "RUN core_tests" "$BUILD/core_tests"
run_stage "CXX opcode_tests" build_core_test opcode_tests
run_stage "RUN opcode_tests" "$BUILD/opcode_tests"

echo
echo "== Architectural integration tests =="
run_stage "CXX architectural_tests" build_core_test architectural_tests
run_stage "RUN architectural_tests" "$BUILD/architectural_tests"
run_stage "CXX bus_integration_tests" build_bus_integration_tests
run_stage "RUN bus_integration_tests" "$BUILD/bus_integration_tests"
run_stage "CXX video_stream_tests" build_video_stream_tests
run_stage "RUN video_stream_tests" "$BUILD/video_stream_tests"
run_stage "CXX video_sync_tests" build_video_sync_tests
run_stage "RUN video_sync_tests" "$BUILD/video_sync_tests"
run_stage "Video converter/integration" python3 tests/video_packer_tests.py
run_stage "CXX audio_stream_tests" build_audio_stream_tests
run_stage "RUN audio_stream_tests" "$BUILD/audio_stream_tests"
run_stage "CXX Rev-A audio SD hooks" build_audio_sd_tests 1
run_stage "RUN Rev-A audio SD hooks" "$BUILD/audio_sd_tests_rev_1"
run_stage "CXX Rev-B audio SD hooks" build_audio_sd_tests 2
run_stage "RUN Rev-B audio SD hooks" "$BUILD/audio_sd_tests_rev_2"
run_stage "CXX Rev-B presence/reset" build_rev_b_bus_tests
run_stage "RUN Rev-B presence/reset" "$BUILD/rev_b_bus_tests"
run_stage "CXX shared QSPI/SD" build_qspi_sd_tests
run_stage "RUN shared QSPI/SD" "$BUILD/qspi_sd_tests"
run_stage "CXX SD/FAT audio source" build_sd_audio_source_tests
run_stage "RUN SD/FAT audio source" "$BUILD/sd_audio_source_tests"

echo
echo "== Synchronization tests =="
run_stage "CXX sync_tests" build_sync_tests
run_stage "RUN sync_tests" "$BUILD/sync_tests"

echo
echo "== FX3 core sanity tests =="
run_stage "CXX fx_core_sanity" build_fx_core_sanity
run_stage "RUN fx_core_sanity" "$BUILD/fx_core_sanity"

echo
echo "== Register/backend tests =="
run_stage "CXX register_backend_tests" build_register_backend_tests
run_stage "RUN register_backend_tests" "$BUILD/register_backend_tests"

echo
echo "== QSPI save journal tests =="
run_stage "CXX save_journal_tests" build_save_journal_tests
run_stage "RUN save_journal_tests" "$BUILD/save_journal_tests"
run_stage "CXX QSPI save integration" build_qspi_save_integration_tests
run_stage "RUN QSPI save integration" "$BUILD/qspi_save_integration_tests"
run_stage "CXX SAVE_AND_STOP integration" build_save_stop_tests
run_stage "RUN SAVE_AND_STOP integration" "$BUILD/save_stop_tests"
run_stage "CXX parallel_rom_programmer" build_parallel_rom_programmer_tests
run_stage "RUN parallel_rom_programmer" "$BUILD/parallel_rom_programmer_tests"
run_stage "CXX SNES ROM layout" build_snes_rom_layout_tests
run_stage "RUN SNES ROM layout" "$BUILD/snes_rom_layout_tests"
run_stage "CXX USB ROM volume" build_usb_rom_volume_tests
run_stage "RUN USB ROM volume" "$BUILD/usb_rom_volume_tests"
run_stage "CXX USB block ordering" build_usb_block_order_tests
run_stage "RUN USB block ordering" "$BUILD/usb_block_order_tests"
run_stage "CXX NOR installer integration" build_snes_rom_installer_tests
run_stage "RUN NOR installer integration" "$BUILD/snes_rom_installer_tests"

echo
echo "== Full production strict stub link =="
run_stage "CXX Rev-A production stub link" build_production_stub
run_stage "CXX Rev-B production stub link" build_production_stub_rev_b

echo
status "All host/static tests" "PASS"
