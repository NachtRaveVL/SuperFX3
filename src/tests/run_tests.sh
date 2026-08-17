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
PICO_INCLUDES=(-Itests/sdk_stubs -I. -Ifx -Iplatform/rp2350)

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

PRODUCTION_SOURCES=(
    main.cpp
    "${CORE_SOURCES[@]}"
    platform/rp2350/fx_backend.cpp
    platform/rp2350/fx_sync.cpp
    platform/rp2350/snes_bus.cpp
    platform/rp2350/snes_pio.cpp
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
        -o "$BUILD/bus_integration_tests"
}

build_bus_integration_tests_dual_rom() {
    "$CXX" "${COMMON_FLAGS[@]}" "${TEST_FLAGS[@]}" -DSNES_PARALLEL_ROM_COUNT=2 "${PICO_INCLUDES[@]}" \
        tests/bus_integration_tests.cpp "${CORE_SOURCES[@]}" platform/rp2350/fx_sync.cpp \
        platform/rp2350/snes_bus.cpp platform/rp2350/snes_pio.cpp \
        -o "$BUILD/bus_integration_tests_dual_rom"
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

build_production_stub() {
    "$CXX" "${COMMON_FLAGS[@]}" "${PICO_INCLUDES[@]}" \
        "${PRODUCTION_SOURCES[@]}" -o "$BUILD/superfx3_stub_link"
}

build_production_stub_dual_rom() {
    "$CXX" "${COMMON_FLAGS[@]}" -DSNES_PARALLEL_ROM_COUNT=2 "${PICO_INCLUDES[@]}" \
        "${PRODUCTION_SOURCES[@]}" -o "$BUILD/superfx3_dual_rom_stub_link"
}

printf "Compiler: %s\n" "$("$CXX" --version | head -n 1)"

echo
echo "== Python/static tests =="
run_stage "PIO static checks" python3 tests/pio_static_tests.py
run_stage "QSPI image packer" python3 tests/packer_tests.py
run_stage "SNES ROM bus image" python3 tests/snes_rom_image_tests.py

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
run_stage "CXX bus_integration dual-ROM" build_bus_integration_tests_dual_rom
run_stage "RUN bus_integration dual-ROM" "$BUILD/bus_integration_tests_dual_rom"

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
echo "== Full production strict stub link =="
run_stage "CXX production stub link" build_production_stub
run_stage "CXX dual-ROM stub link" build_production_stub_dual_rom

echo
status "All host/static tests" "PASS"
