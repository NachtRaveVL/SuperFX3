#pragma once
#define SRAM_BASE 0x20000000u
#define SRAM_END 0x20082000u
#ifdef SDK_TEST_FLASH_EMULATION
#include "../../test_flash.h"
#define XIP_BASE reinterpret_cast<uintptr_t>(sdk_flash::bytes.data())
#else
#define XIP_BASE 0x10000000u
#endif
