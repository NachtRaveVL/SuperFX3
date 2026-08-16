#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../fx/fx_core.h"

#ifndef SUPERFX3_TEST
#error "Host tests must be compiled with SUPERFX3_TEST"
#endif

struct TestSuperFx : SuperFx {
public:
    using SuperFx::cache_;
    using SuperFx::cache_valid_;
    using SuperFx::state_;
    using SuperFx::stopped_;
    using SuperFx::timing_initialized_;
    using SuperFx::wait_for_ram_access_;
    using SuperFx::wait_for_rom_access_;

    using SuperFx::draw_pixel;
    using SuperFx::execute_opcode;
    using SuperFx::flush_primary_cache;
    using SuperFx::fx3_clear;
    using SuperFx::get_color;
    using SuperFx::get_tile_address;
    using SuperFx::get_tile_index;
    using SuperFx::is_transparent_pixel;
    using SuperFx::op_add;
    using SuperFx::op_and_bic;
    using SuperFx::op_asr;
    using SuperFx::op_branch;
    using SuperFx::op_color_cmode;
    using SuperFx::op_dec;
    using SuperFx::op_fmult_lmult;
    using SuperFx::op_from;
    using SuperFx::op_getb;
    using SuperFx::op_getc_ramb_romb;
    using SuperFx::op_hib;
    using SuperFx::op_ibt_sms_lms;
    using SuperFx::op_inc;
    using SuperFx::op_iwt_lm_sm;
    using SuperFx::op_jmp;
    using SuperFx::op_load;
    using SuperFx::op_loop;
    using SuperFx::op_lsr;
    using SuperFx::op_merge;
    using SuperFx::op_mult;
    using SuperFx::op_or_xor;
    using SuperFx::op_rol;
    using SuperFx::op_ror;
    using SuperFx::op_sex;
    using SuperFx::op_stop;
    using SuperFx::op_store;
    using SuperFx::op_sub_compare;
    using SuperFx::op_to;
    using SuperFx::op_with;
    using SuperFx::read_pixel;
    using SuperFx::read_program_byte;
    using SuperFx::read_program_ram;
    using SuperFx::read_ram;
    using SuperFx::read_rom;
    using SuperFx::wait_for_ram_access;
    using SuperFx::wait_for_rom_access;
    using SuperFx::wait_ram_operation;
    using SuperFx::write_pixel_cache;
};

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
