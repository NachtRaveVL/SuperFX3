#pragma once
#include "../test_hardware.h"
using uint = unsigned int;
using irq_handler_t = void(*)();
#define NUM_IRQS sdk_test::IRQ_COUNT
inline void irq_set_exclusive_handler(uint irq, irq_handler_t handler) {
    sdk_test::irq_handler.at(irq) = handler;
}
inline void irq_set_enabled(uint irq, bool enabled) {
    sdk_test::irq_enabled.at(irq) = enabled;
}
inline bool irq_is_enabled(uint irq) { return sdk_test::irq_enabled.at(irq); }
inline void irq_remove_handler(uint irq, irq_handler_t handler) {
    if (sdk_test::irq_handler.at(irq) == handler)
        sdk_test::irq_handler.at(irq) = nullptr;
}
