#pragma once
#include <cstdint>
#define PICO_OK 0
#ifdef SDK_TEST_FLASH_EMULATION
#include "../test_flash.h"
#endif
inline bool flash_safe_execute_core_init() { return true; }
inline int flash_safe_execute(void (*callback)(void*), void* context, uint32_t) {
#ifdef SDK_TEST_FLASH_EMULATION
    if (sdk_flash::fail_enter) return -1;
    sdk_flash::safe = true;
#endif
    callback(context);
#ifdef SDK_TEST_FLASH_EMULATION
    sdk_flash::safe = false;
#endif
    return PICO_OK;
}
