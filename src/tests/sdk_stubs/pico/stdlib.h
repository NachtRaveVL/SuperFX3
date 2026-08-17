#pragma once
#include "pico.h"
#include "../test_hardware.h"
#include <cstdint>
#include <cstdlib>
using uint = unsigned int;
inline void tight_loop_contents() {
    if (sdk_test::tight_loop_hook)
        sdk_test::tight_loop_hook();
}
inline void busy_wait_at_least_cycles(uint32_t cycles) {
    sdk_test::busy_wait_cycles += cycles;
}
[[noreturn]] inline void panic(const char*) { std::abort(); }
