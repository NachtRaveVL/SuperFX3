#pragma once
using uint = unsigned int;
using irq_handler_t = void(*)();
inline void irq_set_exclusive_handler(uint, irq_handler_t) {}
inline void irq_set_enabled(uint, bool) {}
