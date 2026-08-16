#pragma once
struct critical_section_t {};
inline void critical_section_init(critical_section_t*) {}
inline void critical_section_enter_blocking(critical_section_t*) {}
inline void critical_section_exit(critical_section_t*) {}
