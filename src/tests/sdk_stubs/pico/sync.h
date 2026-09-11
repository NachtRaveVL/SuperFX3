#pragma once
#include <cstdint>
inline uint32_t save_and_disable_interrupts() { return 0; }
inline void restore_interrupts(uint32_t) {}
struct critical_section_t {};
inline void critical_section_init(critical_section_t*) {}
inline void critical_section_enter_blocking(critical_section_t*) {}
inline void critical_section_exit(critical_section_t*) {}
