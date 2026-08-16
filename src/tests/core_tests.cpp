#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>

#define private public
#include "../fx/fx_core.h"
#undef private

#include "../fx/fx3_layout.h"
#include "test_support.h"

static uint8_t reference_plane(const uint8_t* pixels, uint8_t plane) {
    uint8_t value = 0;
    for (uint8_t x = 0; x < 8; ++x)
        value |= static_cast<uint8_t>(((pixels[x] >> plane) & 1u) << (7u - x));
    return value;
}

static void write_cache_row(SuperFx& fx, const uint8_t pixels[8], uint8_t y = 0, uint8_t x = 0) {
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
    SuperFx fx;
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
    SuperFx fx;
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
    test_require(fx.state_.r[0] == 0x55AA && fx.cpu_read(0x7300) == 0,
                 "FX3 $x300 open-bus quarter incorrectly reached a register");

    fx.cpu_write(0x7400, 0x11);
    fx.cpu_write(0x7401, 0x22);
    test_require(fx.state_.r[0] == 0x2211 && fx.cpu_read(0x7800) == 0x11,
                 "FX3 $400 register-window mirroring is wrong");
}

static void test_fx3_ignores_ron_ran_waits() {
    TestMemory memory{};
    SuperFx fx;
    fx.init(fx3_config, make_test_backend(memory));

    fx.state_.flags.running = true;
    fx.state_.gsu_rom_access = false;
    fx.state_.gsu_ram_access = false;
    fx.wait_for_rom_access();
    fx.wait_for_ram_access();

    test_require(!fx.wait_for_rom_access_ && !fx.wait_for_ram_access_,
                 "FX3 incorrectly entered a RON/RAN ownership wait");

    SuperFx gsu;
    gsu.init(fx2_config, make_test_backend(memory));
    gsu.state_.flags.running = true;
    gsu.state_.gsu_rom_access = false;
    gsu.wait_for_rom_access();
    test_require(gsu.wait_for_rom_access_,
                 "GSU2 legacy RON ownership wait was accidentally disabled");
}

static void test_blocked_rom_pattern() {
    TestMemory memory{};
    SuperFx fx;
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

int main() {
    test_fx3_8bpp_plot_vectors();
    test_register_start_and_mapping();
    test_fx3_ignores_ron_ran_waits();
    test_blocked_rom_pattern();
    std::puts("core_tests: PASS");
    return 0;
}
