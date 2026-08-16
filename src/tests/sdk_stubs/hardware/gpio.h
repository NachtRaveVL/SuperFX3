#pragma once
#include <cstdint>
#define NUM_BANK0_GPIOS 48
using uint = unsigned int;
static constexpr bool GPIO_IN = false, GPIO_OUT = true; ///< Stub GPIO direction values.
static constexpr int GPIO_FUNC_SIO = 5; ///< Stub SIO function selector.
inline void gpio_init(uint) {}
inline void gpio_disable_pulls(uint) {}
inline void gpio_set_dir(uint, bool) {}
inline void gpio_set_dir_masked64(uint64_t, uint64_t) {}
inline void gpio_put(uint, bool) {}
inline void gpio_put_masked64(uint64_t, uint64_t) {}
inline bool gpio_get(uint) { return true; }
inline uint64_t gpio_get_all64() { return 0; }
inline void gpio_pull_up(uint) {}
inline void gpio_set_function(uint, int) {}
