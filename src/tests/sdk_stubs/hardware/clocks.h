#pragma once
#include <cstdint>
enum clock_index { clk_sys };
inline uint32_t clock_get_hz(clock_index) { return 150000000u; }
