#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>

#include "../fx/fx3_layout.h"
#include "test_support.h"

static uint8_t reference_plane(const uint8_t* pixels, uint8_t plane) {
    uint8_t value = 0;
    for (uint8_t x = 0; x < 8; ++x)
        value |= static_cast<uint8_t>(((pixels[x] >> plane) & 1u) << (7u - x));
    return value;
}

static void write_cache_row(TestSuperFx& fx, const uint8_t pixels[8], uint8_t y = 0, uint8_t x = 0) {
    FxPixelCache cache{};
    cache.x = x;
    cache.y = y;
    cache.valid_bits = 0xFF;

    for (uint8_t pixel = 0; pixel < 8; ++pixel)
        cache.pixels[7u - pixel] = pixels[pixel];

    fx.write_pixel_cache(cache);
}

// Keeps the older fixed/random bit-transpose vectors, but now exercises the actual
// 8bpp GSU pixel-cache path used by the FX3 compatibility implementation.
static void test_fx3_8bpp_plot_vectors() {
    TestMemory memory{};
    TestSuperFx fx;
    fx.init(fx3_config, make_test_backend(memory));

    test_require(fx.state().screen_base == 0x40,
                 "FX3 reset did not initialize SCBR to $40");

    // MD=3 selects 8bpp and HT=1 selects the 160-line / 20-tile-column layout.
    fx.cpu_write(0x703A, 0x07);
    test_require(fx.state().plot_bpp == 8 && fx.state().screen_height == 1,
                 "FX3 SCMR did not select 8bpp 160-line mode");

    const uint8_t known_row[8] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};
    write_cache_row(fx, known_row);

    for (uint8_t plane = 0; plane < 8; ++plane) {
        const uint32_t pair_base = static_cast<uint32_t>(plane >> 1) * 0x10u;
        const uint32_t address = fx3_layout::PLANAR_BASE + pair_base + (plane & 1u);
        test_require(memory.ram[address] == static_cast<uint8_t>(1u << plane),
                     "known 8bpp PLOT vector has the wrong pixel/plane order");
    }

    std::mt19937 rng(0x2350B);
    for (uint32_t iteration = 0; iteration < 128; ++iteration) {
        uint8_t source[8]{};
        for (auto& pixel : source)
            pixel = static_cast<uint8_t>(rng());

        std::fill(memory.ram.begin() + fx3_layout::PLANAR_BASE,
                  memory.ram.begin() + fx3_layout::PLANAR_BASE + 64, 0);
        write_cache_row(fx, source);

        for (uint8_t plane = 0; plane < 8; ++plane) {
            const uint32_t pair_base = static_cast<uint32_t>(plane >> 1) * 0x10u;
            const uint32_t address = fx3_layout::PLANAR_BASE + pair_base + (plane & 1u);
            test_require(memory.ram[address] == reference_plane(source, plane),
                         "random 8bpp PLOT vector disagrees with the naive reference");
        }
    }

    // HT=1 must place the next X tile 20 tiles later, matching FX3 CLEAR geometry.
    const uint8_t next_tile[8] = {0xFF, 0, 0, 0, 0, 0, 0, 0};
    write_cache_row(fx, next_tile, 0, 8);
    const uint32_t next_tile_base = fx3_layout::PLANAR_BASE +
        static_cast<uint32_t>(fx3_layout::PLANAR_Y_TILE_STRIDE) * 64u;
    test_require(memory.ram[next_tile_base] == 0x80,
                 "FX3 160-line PLOT layout does not match the 20-tile CLEAR stride");
}

static void test_register_start_and_mapping() {
    TestMemory memory{};
    TestSuperFx fx;
    fx.init(fx3_config, make_test_backend(memory));

    fx.cpu_write(0x701E, 0x34);
    fx.cpu_write(0x701F, 0x12);
    test_require(fx.running() && fx.state().r[15] == 0x1234,
                 "FX3 alternate register mapping did not start execution through R15");
    test_require(fx.cpu_read(0x701E) == 0x34 && fx.cpu_read(0x701F) == 0x12,
                 "FX3 R15 polling did not use the alternate register mapping");

    fx.op_stop();
    fx.state_.r[0] = 0x55AA;
    fx.cpu_write(0x7300, 0x11);
    fx.cpu_write(0x7301, 0x22);
    test_require(fx.state_.r[0] == 0x55AA && fx.cpu_read(0x7300) == 0xFF,
                 "FX3 $x300 open-bus quarter incorrectly reached a register");

    fx.cpu_write(0x7400, 0x11);
    fx.cpu_write(0x7401, 0x22);
    test_require(fx.state_.r[0] == 0x2211 && fx.cpu_read(0x7800) == 0x11,
                 "FX3 $400 register-window mirroring is wrong");
}

static void test_fx3_ignores_ron_ran_waits() {
    TestMemory memory{};
    TestSuperFx fx;
    fx.init(fx3_config, make_test_backend(memory));

    fx.state_.flags.running = true;
    fx.state_.gsu_rom_access = false;
    fx.state_.gsu_ram_access = false;
    fx.wait_for_rom_access();
    fx.wait_for_ram_access();

    test_require(!fx.wait_for_rom_access_ && !fx.wait_for_ram_access_,
                 "FX3 incorrectly entered a RON/RAN ownership wait");

    TestSuperFx gsu;
    gsu.init(fx2_config, make_test_backend(memory));
    gsu.state_.flags.running = true;
    gsu.state_.gsu_rom_access = false;
    gsu.wait_for_rom_access();
    test_require(gsu.wait_for_rom_access_,
                 "GSU2 legacy RON ownership wait was accidentally disabled");
}

static void test_blocked_rom_pattern() {
    TestMemory memory{};
    TestSuperFx fx;
    fx.init(fx2_config, make_test_backend(memory));

    for (uint32_t address = 0; address < 16; ++address) {
        uint8_t expected = 0;
        if (address & 1u) expected = 1;
        else if ((address & 0x0Eu) == 0x04u) expected = 0x04;
        else if ((address & 0x0Eu) == 0x0Au) expected = 0x08;
        else if ((address & 0x0Eu) == 0x0Eu) expected = 0x0C;
        test_require(fx.blocked_rom_value(address) == expected,
                     "blocked ROM pattern differs from the expected GSU pattern");
    }
}

static void test_gsu_memory_mapping_and_waits() {
    TestMemory memory{};
    TestSuperFx fx;
    fx.init(fx3_config, make_test_backend(memory));

    uint32_t offset = 0;
    test_require(fx.gsu_rom_offset(0x000000, offset) && offset == 0x000000,
                 "GSU low-half ROM mirror is wrong");
    test_require(fx.gsu_rom_offset(0x008000, offset) && offset == 0x000000,
                 "GSU upper-half ROM mapping is wrong");
    test_require(fx.gsu_rom_offset(0x3FFFFF, offset) && offset == 0x1FFFFF,
                 "GSU 32 KiB bank mapping is wrong at the 2 MiB boundary");
    test_require(fx.gsu_rom_offset(0x600000, offset) && offset == 0x200000,
                 "FX3 extended ROM bank $60 mapping is wrong");
    test_require(fx.gsu_rom_offset(0x6FFFFF, offset) && offset == 0x2FFFFF,
                 "FX3 extended ROM bank $6F mapping is wrong");
    test_require(!fx.gsu_rom_offset(0x700000, offset),
                 "GSU ROM mapper accepted program RAM as ROM");

    TestSuperFx gsu2;
    gsu2.init(fx2_config, make_test_backend(memory));
    test_require(!gsu2.gsu_rom_offset(0x600000, offset),
                 "GSU2 ROM mapper incorrectly accepted the FX3 extension");

    gsu2.state_.flags.running = true;
    gsu2.state_.gsu_rom_access = false;
    (void)gsu2.read_rom(0x400000);
    test_require(gsu2.wait_for_rom_access_ && gsu2.stopped_,
                 "read_rom did not enter the legacy ROM ownership wait");

    TestSuperFx gsu2_ram;
    gsu2_ram.init(fx2_config, make_test_backend(memory));
    gsu2_ram.state_.flags.running = true;
    gsu2_ram.state_.gsu_ram_access = false;
    (void)gsu2_ram.read_ram(0x0000);
    test_require(gsu2_ram.wait_for_ram_access_ && gsu2_ram.stopped_,
                 "read_ram did not enter the legacy RAM ownership wait");

    memory.rom[0x000123] = 0x11;
    memory.rom[0x400123] = 0x22;
    test_require(fx.read_rom(0x400123) == 0x11,
                 "GSU ROM read did not translate to a linear backend offset");
    test_require(fx.cpu_rom_read(0x400123) == 0x22,
                 "SNES CPU ROM read incorrectly used the GSU ROM mapper");

    fx.state_.rom_bank = 0x40;
    fx.state_.r[14] = 0x0040;
    fx.state_.rom_delay = 3;
    fx.state_.flags.rom_read_pending = true;
    memory.rom[0x0040] = 0xA5;
    memory.rom[0x0080] = 0x5A;
    const uint64_t rom_cycles = fx.state_.cycles;
    test_require(fx.read_rom(0x400080) == 0x5A,
                 "ROM read after a pending R14 operation returned the wrong byte");
    test_require(fx.state_.cycles == rom_cycles + 3 && fx.state_.rom_read_buffer == 0xA5 &&
                     !fx.state_.flags.rom_read_pending,
                 "read_rom did not retire the pending ROM operation first");

    fx.state_.ram_bank = 1;
    fx.state_.ram_write_address = 0x1234;
    fx.state_.ram_write_value = 0xC7;
    fx.state_.ram_delay = 4;
    memory.ram[0x01234] = 0x18;
    memory.ram[0x11234] = 0x29;
    const uint64_t ram_cycles = fx.state_.cycles;
    test_require(fx.read_ram(0x1234) == 0xC7,
                 "RAMB-relative read did not observe the completed pending write");
    test_require(fx.state_.cycles == ram_cycles + 4,
                 "read_ram did not retire the pending RAM operation first");

    fx.state_.ram_bank = 0;
    memory.ram[0x10055] = 0x6E;
    test_require(fx.read_program_ram(0x710055) == 0x6E,
                 "$71 program RAM read incorrectly applied RAMB");
    test_require(fx.read_program_ram(0x720055) == 0xFF,
                 "unmapped program RAM read did not return the open-bus value");
}

int main() {
    test_fx3_8bpp_plot_vectors();
    test_register_start_and_mapping();
    test_fx3_ignores_ron_ran_waits();
    test_blocked_rom_pattern();
    test_gsu_memory_mapping_and_waits();
    std::puts("core_tests: PASS");
    return 0;
}
