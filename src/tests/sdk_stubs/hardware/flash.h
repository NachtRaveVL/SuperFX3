#pragma once
#include <cstddef>
#include <cstdint>
#define FLASH_PAGE_SIZE 256u
#define FLASH_SECTOR_SIZE 4096u
#ifdef SDK_TEST_FLASH_EMULATION
#include "../test_flash.h"
#include <algorithm>
#include <cassert>
inline void flash_range_erase(uint32_t offset, size_t size) {
    assert(sdk_flash::safe && offset % FLASH_SECTOR_SIZE == 0 && size % FLASH_SECTOR_SIZE == 0);
#ifdef SDK_TEST_FX_ROM_PROGRAMMING
    assert(offset >= 0x100000 && offset + size <= 0x400000);
#else
    assert(offset >= 0x7C000 && offset + size <= 0x100000);
#endif
    assert(sdk_flash::other_core_parked && sdk_flash::interrupts_disabled);
    sdk_flash::xip = false;
    if (sdk_flash::check_mutation) sdk_flash::check_mutation();
    std::fill_n(sdk_flash::bytes.begin() + offset, size, 0xFF);
    ++sdk_flash::erases;
    sdk_flash::xip = true;
}
inline void flash_range_program(uint32_t offset, const uint8_t* bytes, size_t size) {
    assert(sdk_flash::safe && offset % FLASH_PAGE_SIZE == 0 && size % FLASH_PAGE_SIZE == 0);
#ifdef SDK_TEST_FX_ROM_PROGRAMMING
    assert(offset >= 0x100000 && offset + size <= 0x400000);
#else
    assert(offset >= 0x7C000 && offset + size <= 0x100000);
#endif
    assert(sdk_flash::other_core_parked && sdk_flash::interrupts_disabled);
    sdk_flash::xip = false;
    if (sdk_flash::check_mutation) sdk_flash::check_mutation();
    ++sdk_flash::programs;
    if (!sdk_flash::corrupt_program) {
        for (size_t i = 0; i < size; ++i)
            sdk_flash::bytes[offset + i] &= bytes[i];
    }
    sdk_flash::xip = true;
}
#else
inline void flash_range_erase(uint32_t, size_t) {}
inline void flash_range_program(uint32_t, const uint8_t*, size_t) {}
#endif
