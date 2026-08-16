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
CORE_INCLUDES=(-Itests/stubs -I. -Ifx)
PICO_INCLUDES=(-Itests/sdk_stubs -I. -Ifx -Iplatform/rp2350)
PICO_DEFINES=(-DPICO_FLASH_SIZE_BYTES=4194304)

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

build_and_run_core_test() {
    local name="$1"
    "$CXX" "${COMMON_FLAGS[@]}" "${CORE_INCLUDES[@]}" \
        "tests/$name.cpp" "${CORE_SOURCES[@]}" -o "$BUILD/$name"
    "$BUILD/$name"
}

echo "== Python/static tests =="
python3 tests/pio_static_tests.py
python3 tests/packer_tests.py

echo
echo "== Portable core tests =="
build_and_run_core_test core_tests
build_and_run_core_test opcode_tests

echo
echo "== Synchronization tests =="
"$CXX" "${COMMON_FLAGS[@]}" "${PICO_INCLUDES[@]}" "${PICO_DEFINES[@]}" \
    tests/sync_tests.cpp "${CORE_SOURCES[@]}" platform/rp2350/fx_sync.cpp \
    -o "$BUILD/sync_tests"
"$BUILD/sync_tests"

echo
echo "== FX3/PDF sanity tests =="
"$CXX" "${COMMON_FLAGS[@]}" "${PICO_INCLUDES[@]}" "${PICO_DEFINES[@]}" \
    tests/fx_core_sanity.cpp "${CORE_SOURCES[@]}" platform/rp2350/fx_backend.cpp \
    tests/sdk_stubs/flash_end.cpp -o "$BUILD/fx_core_sanity"
"$BUILD/fx_core_sanity"

echo
echo "== Register/backend tests =="
"$CXX" "${COMMON_FLAGS[@]}" "${PICO_INCLUDES[@]}" "${PICO_DEFINES[@]}" \
    tests/register_backend_tests.cpp "${CORE_SOURCES[@]}" platform/rp2350/fx_backend.cpp \
    tests/sdk_stubs/flash_end.cpp -o "$BUILD/register_backend_tests"
"$BUILD/register_backend_tests"

echo
echo "== Full production strict stub link =="
"$CXX" "${COMMON_FLAGS[@]}" "${PICO_INCLUDES[@]}" "${PICO_DEFINES[@]}" \
    "${PRODUCTION_SOURCES[@]}" -o "$BUILD/superfx3_stub_link"

echo
echo "All host/static tests: PASS"
