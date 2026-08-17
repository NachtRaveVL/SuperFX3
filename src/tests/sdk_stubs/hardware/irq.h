#pragma once
#include "../test_hardware.h"
using uint = unsigned int;
using irq_handler_t = void(*)();
inline void irq_set_exclusive_handler(uint irq, irq_handler_t handler) {
    sdk_test::irq_handler.at(irq) = handler;
}
inline void irq_set_enabled(uint irq, bool enabled) {
    sdk_test::irq_enabled.at(irq) = enabled;
}
