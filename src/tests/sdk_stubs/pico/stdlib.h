#pragma once
#include <cstdint>
#include <cstdlib>
using uint = unsigned int;
inline void tight_loop_contents() {}
inline void busy_wait_at_least_cycles(uint32_t) {}
[[noreturn]] inline void panic(const char*) { std::abort(); }
