/*
 * NR-RetroWorks SuperFX3 Firmware - host-side core sanity tests
 *
 * These tests intentionally exercise only architecture-independent core behavior.
 * They do not prove SNES bus timing, PIO ordering, cross-core liveness, or electrical
 * bus ownership. Those require separate RP2350/SNES hardware tests.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../fx/fx3_layout.h"
#include "../platform/rp2350/fx_backend.h"
#include "test_support.h"

static uint8_t reference_plane(const uint8_t pixels[8], uint8_t plane) {
    uint8_t value = 0;
    for (uint8_t x = 0; x < 8; ++x)
        value |= static_cast<uint8_t>(((pixels[x] >> plane) & 1u) << (7u - x));
    return value;
}

static bool test_blocked_rom_pattern(TestSuperFx& fx) {
    static constexpr uint8_t expected[16] = { ///< Expected blocked-ROM pattern for A0-A3.
        0x00, 0x01, 0x00, 0x01,
        0x04, 0x01, 0x00, 0x01,
        0x00, 0x01, 0x08, 0x01,
        0x00, 0x01, 0x0C, 0x01
    };

    for (uint32_t address = 0; address < 16; ++address) {
        if (fx.blocked_rom_value(address) != expected[address]) {
            std::printf("Blocked-ROM mismatch at A0-A3=%u\n", address);
            return false;
        }
    }

    return true;
}

static bool test_fx3_clear(TestSuperFx& fx, TestMemory& memory) {
    static constexpr uint8_t clear_pattern[64] = { ///< Expected FX3 patterned-clear tile payload.
        0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF
    };

    struct ClearRange {
        uint8_t first;
        uint8_t last;
    };

    static constexpr ClearRange ranges[3] = { ///< Expected tile-column ranges for clear commands 3-5.
        {0, 8}, {9, 17}, {18, 26}
    };

    for (const ClearRange range : ranges) {
        std::fill(memory.ram.begin() + fx3_layout::PLANAR_BASE,
                  memory.ram.end(), 0x5A);
        fx.fx3_clear(range.first, range.last);

        for (uint32_t x_tile = 0; x_tile < fx3_layout::X_TILES; ++x_tile) {
            const bool should_clear = x_tile >= range.first && x_tile <= range.last;

            for (uint32_t y_tile = 0; y_tile < fx3_layout::PLANAR_Y_TILE_STRIDE; ++y_tile) {
                const uint32_t base = fx3_layout::PLANAR_BASE +
                    (x_tile * fx3_layout::PLANAR_Y_TILE_STRIDE + y_tile) * 64u;

                for (uint32_t i = 0; i < 64; ++i) {
                    const uint8_t expected =
                        should_clear && y_tile < fx3_layout::Y_TILES ? clear_pattern[i] : 0x5A;

                    if (memory.ram[base + i] != expected) {
                        std::printf(
                            "Clear mismatch: range=%u-%u xt=%u yt=%u i=%u got=%02X expected=%02X\n",
                            range.first, range.last, x_tile, y_tile, i,
                            memory.ram[base + i], expected
                        );
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

static bool test_fx3_plot_pixel_cache(TestSuperFx& fx, TestMemory& memory) {
    fx.reset();

    // MD=3 selects 8bpp. HT=1 selects the 160-line layout whose columns are
    // 20 tiles apart, matching the FX3 clear-command storage geometry.
    fx.cpu_write(0x703A, 0x07);
    fx.state_.plot_transparent = true;
    fx.state_.plot_dither = false;

    const uint8_t pixels[8] = {0x80, 0x41, 0x22, 0x13, 0x0C, 0x05, 0x02, 0xFF};
    for (uint8_t x = 0; x < 8; ++x) {
        fx.state_.color = pixels[x];
        fx.draw_pixel(x, 0);
    }

    // A full primary cache is handed to the secondary slot before writeback.
    fx.write_pixel_cache(fx.state_.secondary_cache);

    for (uint8_t plane = 0; plane < 8; ++plane) {
        const uint32_t offset = static_cast<uint32_t>(plane >> 1) * 0x10u + (plane & 1u);
        const uint8_t expected = reference_plane(pixels, plane);
        if (memory.ram[fx3_layout::PLANAR_BASE + offset] != expected) {
            std::printf("FX3 8bpp PLOT plane %u mismatch: got %02X expected %02X\n",
                        plane, memory.ram[fx3_layout::PLANAR_BASE + offset], expected);
            return false;
        }
    }

    for (uint8_t x = 0; x < 8; ++x) {
        if (fx.read_pixel(x, 0) != pixels[x]) {
            std::printf("FX3 RPIX round-trip mismatch at x=%u\n", x);
            return false;
        }
    }

    // The next X tile must use the same 20-tile column stride used by CLEAR.
    static constexpr uint8_t next_pixels[8] = {0xFF, 0, 0, 0, 0, 0, 0, 0}; ///< Second pixel-cache test row.
    for (uint8_t x = 0; x < 8; ++x) {
        fx.state_.color = next_pixels[x];
        fx.draw_pixel(static_cast<uint8_t>(8u + x), 0);
    }
    fx.write_pixel_cache(fx.state_.secondary_cache);

    const uint32_t next_tile_base = fx3_layout::PLANAR_BASE +
        static_cast<uint32_t>(fx3_layout::PLANAR_Y_TILE_STRIDE) * 64u;
    if (memory.ram[next_tile_base] != 0x80) {
        std::puts("FX3 PLOT and CLEAR disagree on the 160-line planar column stride");
        return false;
    }

    return true;
}

static bool test_fx3_qspi_rom_backend() {
    std::vector<uint8_t> rom(3u << 20, 0);
    rom[0x000000] = 0x11;
    rom[0x1FFFFF] = 0x22;
    rom[0x200000] = 0x33;
    rom[0x2FFFFF] = 0x44;

    Rp2350FxBackendContext context {
        rom.data(), static_cast<uint32_t>(rom.size()), 0,
        nullptr, fx3_qspi_rom_read, nullptr
    };

    if (fx3_qspi_rom_read(&context, 0x000000) != 0x11 ||
        fx3_qspi_rom_read(&context, 0x1FFFFF) != 0x22 ||
        fx3_qspi_rom_read(&context, 0x200000) != 0x33 ||
        fx3_qspi_rom_read(&context, 0x2FFFFF) != 0x44) {
        std::puts("FX3 QSPI backend did not read linear ROM offsets");
        return false;
    }

    if (fx3_qspi_rom_read(&context, 0x300000) != 0xFF) {
        std::puts("FX3 QSPI backend out-of-range fallback is wrong");
        return false;
    }

    return true;
}

static bool test_fx3_merge_dispatch(TestSuperFx& fx, TestMemory& memory) {
    // The retained PLOT/pixel-cache path already performs C2P, so commands 0-2
    // must be harmless. Commands 3-5 retain their patterned clear behavior.
    for (uint16_t command = 0; command <= 5; ++command) {
        fx.reset();
        std::fill(memory.ram.begin() + fx3_layout::PLANAR_BASE, memory.ram.end(), 0x5A);
        const std::vector<uint8_t> before = memory.ram;

        fx.state_.r[0] = command;
        fx.op_merge();

        if (command <= 2) {
            if (memory.ram != before) {
                std::printf("FX3 MERGE C2P command %u was not a no-op\n", command);
                return false;
            }
        } else {
            const uint32_t first_tile = static_cast<uint32_t>(command - 3) * 9u;
            const uint32_t address = fx3_layout::PLANAR_BASE +
                first_tile * fx3_layout::PLANAR_Y_TILE_STRIDE * 64u;
            if (memory.ram[address] != 0xFF) {
                std::printf("FX3 MERGE clear command %u did not modify its expected region\n", command);
                return false;
            }
        }
    }

    const uint8_t before = memory.ram[fx3_layout::PLANAR_BASE + 0x1234];
    fx.state_.r[0] = 0xFFFF;
    fx.op_merge();
    if (memory.ram[fx3_layout::PLANAR_BASE + 0x1234] != before) {
        std::puts("Unknown FX3 MERGE command modified framebuffer RAM");
        return false;
    }

    return true;
}

static bool test_fx3_primary_spec_rules(TestSuperFx& fx, TestMemory& memory) {
    fx.reset();

    if (fx.config().max_program_rom_bank != 0x6F) {
        std::puts("FX3 program ROM bank limit is not $6F");
        return false;
    }

    if (fx.state_.screen_base != 0x40) {
        std::puts("FX3 reset did not initialize SCBR to $40");
        return false;
    }

    if (fx.cpu_read(0x703B) != 0x52) {
        std::puts("FX3 VCR did not return $52");
        return false;
    }

    // FX3 allows simultaneous FX/65816 ROM and SRAM access and ignores RON/RAN
    // as FX-side access gates. GSU1/2 retain the original ownership waits.
    fx.state_.flags.running = true;
    fx.state_.gsu_rom_access = false;
    fx.state_.gsu_ram_access = false;

    memory.rom[0x1234] = 0xA6;
    memory.ram[0x1234] = 0x5C;
    if (!fx.rom_access_allowed() || !fx.ram_access_allowed() ||
        fx.cpu_rom_read(0x1234) != 0xA6 || fx.cpu_ram_read(0x1234) != 0x5C) {
        std::puts("FX3 incorrectly gated simultaneous CPU ROM/RAM access");
        return false;
    }

    fx.wait_for_rom_access();
    fx.wait_for_ram_access();
    if (fx.wait_for_rom_access_ || fx.wait_for_ram_access_) {
        std::puts("FX3 incorrectly stalled its own access on RON/RAN");
        return false;
    }

    fx.cpu_ram_write(0x1234, 0xD3);
    if (memory.ram[0x1234] != 0xD3) {
        std::puts("FX3 blocked simultaneous CPU FX-SRAM write access");
        return false;
    }

    fx.state_.flags.running = false;
    return true;
}

static bool test_fx3_register_basics(TestSuperFx& fx, TestMemory& memory) {
    fx.reset();

    fx.cpu_write(0x7000, 0x34);
    fx.cpu_write(0x7001, 0x12);
    if (fx.cpu_read(0x7000) != 0x34 || fx.cpu_read(0x7001) != 0x12) {
        std::puts("FX3 register-window relocation failed");
        return false;
    }

    fx.cpu_write(0x72FF, 0xC3);
    if (fx.cpu_read(0x72FF) != 0xC3) {
        std::puts("FX3 register window did not include $72FF");
        return false;
    }

    fx.state_.r[0] = 0xA55A;
    fx.cpu_write(0x7300, 0x11);
    fx.cpu_write(0x7301, 0x22);
    if (fx.state_.r[0] != 0xA55A || fx.cpu_read(0x7300) != 0xFF) {
        std::puts("FX3 $x300 open-bus quarter incorrectly reached a register");
        return false;
    }

    fx.cpu_write(0x7400, 0x11);
    fx.cpu_write(0x7401, 0x22);
    if (fx.state_.r[0] != 0x2211 || fx.cpu_read(0x7800) != 0x11) {
        std::puts("FX3 $400 register-window mirroring failed");
        return false;
    }

    // Writing R15 high starts execution; while running FX3 still exposes R15.
    fx.cpu_write(0x701E, 0x78);
    fx.cpu_write(0x701F, 0x56);
    if (!fx.running() || fx.cpu_read(0x701E) != 0x78 || fx.cpu_read(0x701F) != 0x56) {
        std::puts("FX3 R15 start/poll behavior failed");
        return false;
    }

    if (fx.cpu_read(0x7000) != 0xFF) {
        std::puts("Running-state register gate failed");
        return false;
    }

    memory.irq = false;
    fx.op_stop();
    if (fx.running() || fx.cpu_read(0x701E) != 0 || fx.cpu_read(0x701F) != 0 || memory.irq) {
        std::puts("FX3 STOP completion behavior failed");
        return false;
    }

    return true;
}

int main() {
    TestMemory memory{};
    TestSuperFx fx;
    fx.init(fx3_config, make_test_backend(memory));

    if (!test_blocked_rom_pattern(fx)) return 1;
    if (!test_fx3_clear(fx, memory)) return 2;
    if (!test_fx3_plot_pixel_cache(fx, memory)) return 3;
    if (!test_fx3_qspi_rom_backend()) return 4;
    if (!test_fx3_merge_dispatch(fx, memory)) return 5;
    if (!test_fx3_primary_spec_rules(fx, memory)) return 6;
    if (!test_fx3_register_basics(fx, memory)) return 7;

    std::puts("fx_core_sanity: PASS");
    return 0;
}
