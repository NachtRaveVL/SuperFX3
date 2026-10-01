#pragma once
#include <array>
#include <cstdint>

namespace sdk_flash {
inline std::array<uint8_t, 4u * 1024u * 1024u> bytes{};
inline bool safe = false;
inline bool xip = true;
inline bool other_core_parked = false;
inline bool interrupts_disabled = false;
inline void (*check_locked)() = nullptr;
inline bool fail_enter = false;
inline bool corrupt_program = false;
inline uint32_t programs = 0;
inline uint32_t erases = 0;
inline void (*check_mutation)() = nullptr;
}
