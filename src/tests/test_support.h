#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../fx/fx_core.h"

struct TestMemory {
    std::vector<uint8_t> rom = std::vector<uint8_t>(8u * 1024u * 1024u);
    std::vector<uint8_t> ram = std::vector<uint8_t>(128u * 1024u);
    bool irq = false;
};

inline uint8_t test_rom_read(void* context, uint32_t address) {
    auto& memory = *static_cast<TestMemory*>(context);
    return address < memory.rom.size() ? memory.rom[address] : 0xFF;
}

inline uint8_t test_ram_read(void* context, uint32_t address) {
    auto& memory = *static_cast<TestMemory*>(context);
    return address < memory.ram.size() ? memory.ram[address] : 0xFF;
}

inline void test_ram_write(void* context, uint32_t address, uint8_t value) {
    auto& memory = *static_cast<TestMemory*>(context);
    if (address < memory.ram.size())
        memory.ram[address] = value;
}

inline void test_set_irq(void* context, bool asserted) {
    static_cast<TestMemory*>(context)->irq = asserted;
}

inline FxBackend make_test_backend(TestMemory& memory) {
    return FxBackend{
        &memory,
        test_rom_read,
        test_ram_read,
        test_ram_write,
        test_set_irq
    };
}

[[noreturn]] inline void test_fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

inline void test_require(bool condition, const char* message) {
    if (!condition)
        test_fail(message);
}
