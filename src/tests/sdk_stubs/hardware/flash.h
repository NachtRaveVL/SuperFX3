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
    assert(offset >= 0x80000 && offset + size <= 0x100000);
    if (sdk_flash::check_busy) sdk_flash::check_busy();
    std::fill_n(sdk_flash::bytes.begin() + offset, size, 0xFF);
    ++sdk_flash::erases;
}
inline void flash_range_program(uint32_t offset, const uint8_t* bytes, size_t size) {
    assert(sdk_flash::safe && offset % FLASH_PAGE_SIZE == 0 && size % FLASH_PAGE_SIZE == 0);
    assert(offset >= 0x80000 && offset + size <= 0x100000);
    if (sdk_flash::check_busy) sdk_flash::check_busy();
    ++sdk_flash::programs;
    if (!sdk_flash::corrupt_program) {
        for (size_t i = 0; i < size; ++i)
            sdk_flash::bytes[offset + i] &= bytes[i];
    }
}
#else
inline void flash_range_erase(uint32_t, size_t) {}
inline void flash_range_program(uint32_t, const uint8_t*, size_t) {}
#endif
