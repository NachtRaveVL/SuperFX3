#pragma once
#include <cstdint>
#include "../test_hardware.h"
#define NUM_BANK0_GPIOS 48
using uint = unsigned int;
static constexpr bool GPIO_IN = false, GPIO_OUT = true; ///< Stub GPIO direction values.
static constexpr int GPIO_FUNC_SIO = 5; ///< Stub SIO function selector.
inline void gpio_init(uint pin) {
    sdk_test::gpio_dir.at(pin) = GPIO_IN;
}
inline void gpio_disable_pulls(uint pin) {
    sdk_test::gpio_pullup.at(pin) = false;
}
inline void gpio_set_dir(uint pin, bool dir) {
    sdk_test::gpio_dir.at(pin) = dir;
}
inline void gpio_set_dir_masked64(uint64_t mask, uint64_t values) {
    for (uint pin = 0; pin < sdk_test::GPIO_COUNT; ++pin) {
        const uint64_t bit = uint64_t{1} << pin;
        if (mask & bit)
            sdk_test::gpio_dir[pin] = (values & bit) != 0;
    }
}
inline void gpio_put(uint pin, bool value) {
    sdk_test::gpio_level.at(pin) = value;
    ++sdk_test::gpio_put_count.at(pin);
    if (!value)
        ++sdk_test::gpio_low_count.at(pin);
}
inline void gpio_put_masked64(uint64_t mask, uint64_t values) {
    sdk_test::set_gpio_mask(mask, values);
}
inline bool gpio_get(uint pin) {
    return sdk_test::gpio_level.at(pin);
}
inline uint64_t gpio_get_all64() {
    return sdk_test::gpio_snapshot();
}
inline void gpio_pull_up(uint pin) {
    sdk_test::gpio_pullup.at(pin) = true;
    sdk_test::gpio_level.at(pin) = true;
}
inline void gpio_set_function(uint pin, int function) {
    sdk_test::gpio_function.at(pin) = function;
}
