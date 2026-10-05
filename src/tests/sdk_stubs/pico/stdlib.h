#pragma once
#include "pico.h"
#include "../test_hardware.h"
#include <cstdint>
#include <cstdlib>
#include <chrono>
using uint = unsigned int;
inline void tight_loop_contents() {
    if (sdk_test::tight_loop_hook)
        sdk_test::tight_loop_hook();
}
inline void busy_wait_at_least_cycles(uint32_t cycles) {
    sdk_test::busy_wait_cycles += cycles;
    if (sdk_test::busy_wait_hook)
        sdk_test::busy_wait_hook();
}
inline uint32_t time_us_32() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
[[noreturn]] inline void panic(const char*) { std::abort(); }
