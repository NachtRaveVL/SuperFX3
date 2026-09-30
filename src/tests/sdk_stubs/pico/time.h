#pragma once
#include <cstdint>
#include "../test_hardware.h"
inline uint64_t time_us_64() { return ++sdk_test::busy_wait_cycles; }
