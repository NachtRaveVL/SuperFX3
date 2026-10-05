#pragma once
#include <cstdint>
#ifdef SDK_TEST_THREADED_CRITICAL_SECTIONS
#include <mutex>
#endif
inline uint32_t save_and_disable_interrupts() { return 0; }
inline void restore_interrupts(uint32_t) {}
inline void __dsb() {}
inline void __isb() {}
#ifdef SDK_TEST_THREADED_CRITICAL_SECTIONS
struct critical_section_t { std::mutex gate; };
inline void critical_section_init(critical_section_t*) {}
inline void critical_section_enter_blocking(critical_section_t* section) { section->gate.lock(); }
inline void critical_section_exit(critical_section_t* section) { section->gate.unlock(); }
#else
struct critical_section_t {};
inline void critical_section_init(critical_section_t*) {}
inline void critical_section_enter_blocking(critical_section_t*) {}
inline void critical_section_exit(critical_section_t*) {}
#endif
