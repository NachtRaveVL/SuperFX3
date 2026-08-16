#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/coverage"
OBJ="$BUILD/obj"
CXX="${CXX:-g++}"
GCOV="${GCOV:-gcov}"

cd "$ROOT"
rm -rf "$BUILD"
mkdir -p "$OBJ"

FLAGS=(
    -std=c++17
    -O0 -g
    --coverage
    -Wall -Wextra -Wpedantic -Werror
    -Wconversion -Wsign-conversion
    -DSUPERFX3_TEST
    -Itests/sdk_stubs -Itests/stubs -I. -Ifx -Iplatform/rp2350
)

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
COVERED_SOURCES=(
    "${CORE_SOURCES[@]}"
    platform/rp2350/fx_sync.cpp
    platform/rp2350/fx_backend.cpp
)

for source in "${COVERED_SOURCES[@]}"; do
    object="$OBJ/$(basename "${source%.cpp}").o"
    "$CXX" "${FLAGS[@]}" -c "$source" -o "$object"
done

CORE_OBJECTS=()
for source in "${CORE_SOURCES[@]}"; do
    CORE_OBJECTS+=("$OBJ/$(basename "${source%.cpp}").o")
done
SYNC_OBJECT="$OBJ/fx_sync.o"
BACKEND_OBJECT="$OBJ/fx_backend.o"

build_and_run() {
    local name="$1"
    shift
    "$CXX" "${FLAGS[@]}" "tests/$name.cpp" "$@" -o "$BUILD/$name"
    "$BUILD/$name"
}

# Each test links against the same instrumented production objects. Their .gcda files
# accumulate coverage across the complete suite rather than measuring one executable.
build_and_run core_tests "${CORE_OBJECTS[@]}"
build_and_run opcode_tests "${CORE_OBJECTS[@]}"
build_and_run sync_tests "${CORE_OBJECTS[@]}" "$SYNC_OBJECT"
build_and_run fx_core_sanity "${CORE_OBJECTS[@]}" "$BACKEND_OBJECT" tests/sdk_stubs/flash_end.cpp
build_and_run register_backend_tests "${CORE_OBJECTS[@]}" "$BACKEND_OBJECT" tests/sdk_stubs/flash_end.cpp

GCOV_OUTPUT="$BUILD/gcov.txt"
: > "$GCOV_OUTPUT"
for source in "${COVERED_SOURCES[@]}"; do
    "$GCOV" -n -b -c -o "$OBJ" "$source" >> "$GCOV_OUTPUT"
done

REPORT="$BUILD/coverage_report.txt"
python3 tests/coverage_summary.py "$GCOV_OUTPUT" | tee "$REPORT"

echo
echo "Coverage report: $REPORT"
