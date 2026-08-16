#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

#define private public
#include "../fx/fx_core.h"
#undef private

#include "../platform/rp2350/fx_backend.h"
#include "test_support.h"

static bool backend_irq_level = false;

static void backend_irq_write(void*, bool asserted) {
    backend_irq_level = asserted;
}

static void test_register_reads_and_writes() {
    TestMemory memory{};
    SuperFx fx;
    fx.init(fx2_config, make_test_backend(memory));

    // General register latch/commit, including R14 buffered-ROM side effect.
    fx.cpu_write(0x3000, 0x34);
    fx.cpu_write(0x3001, 0x12);
    test_require(fx.cpu_read(0x3000) == 0x34 && fx.cpu_read(0x3001) == 0x12,
                 "general register low/high access is wrong");

    fx.cpu_write(0x301C, 0x78);
    fx.cpu_write(0x301D, 0x56);
    test_require(fx.state_.r[14] == 0x5678 && fx.state_.flags.rom_read_pending,
                 "R14 write did not start buffered ROM read");

    // Exercise every readable special register and cache addressing.
    fx.state_.program_bank = 0x22;
    fx.state_.rom_bank = 0x33;
    fx.state_.ram_bank = 1;
    fx.state_.cache_base = 0x01F0;
    fx.cache_[0x1F0] = 0x9A;
    test_require(fx.cpu_read(0x3034) == 0x22, "PBR read is wrong");
    test_require(fx.cpu_read(0x3036) == 0x33, "ROMBR read is wrong");
    test_require(fx.cpu_read(0x303B) == 0x04, "GSU VCR read is wrong");
    test_require(fx.cpu_read(0x303C) == 1, "RAMBR read is wrong");
    test_require(fx.cpu_read(0x303E) == 0xF0 && fx.cpu_read(0x303F) == 0x01,
                 "CBR read is wrong");
    test_require(fx.cpu_read(0x3100) == 0x9A, "cache-window read is wrong");
    test_require(fx.cpu_read(0x30AA) == 0, "unmapped register read fallback is wrong");

    // SFR read packs flags and clears an existing IRQ.
    fx.state_.flags.zero = true;
    fx.state_.flags.carry = true;
    fx.state_.flags.sign = true;
    fx.state_.flags.overflow = true;
    fx.state_.flags.rom_read_pending = true;
    fx.state_.flags.alt1 = true;
    fx.state_.flags.alt2 = true;
    fx.state_.flags.imm_low = true;
    fx.state_.flags.imm_high = true;
    fx.state_.flags.prefix = true;
    fx.state_.flags.irq = true;
    memory.irq = true;
    test_require((fx.cpu_read(0x3030) & 0x5E) == 0x5E, "SFR low packing is wrong");
    const uint8_t high = fx.cpu_read(0x3031);
    test_require((high & 0x9F) == 0x9F && !fx.state_.flags.irq && !memory.irq,
                 "SFR high packing/IRQ clear is wrong");

    // Special-register writes.
    fx.cpu_write(0x3033, 1);
    test_require(fx.state_.backup_ram_enabled, "BRAMR write is wrong");
    fx.cache_valid_[0] = true;
    fx.cpu_write(0x3034, 0xFF);
    test_require(fx.state_.program_bank == 0x7F && !fx.cache_valid_[0],
                 "PBR write/mask/cache invalidation is wrong");
    fx.cpu_write(0x3037, 0xA0);
    test_require(fx.state_.high_speed && fx.state_.irq_disabled, "CFGR write is wrong");
    fx.cpu_write(0x3038, 0x44);
    test_require(fx.state_.screen_base == 0x44, "SCBR write is wrong");
    fx.cpu_write(0x3039, 1);
    test_require(fx.state_.clock_select, "CLSR write is wrong");

    static constexpr uint8_t scmr_values[] = {0x00, 0x01, 0x02, 0x03, 0x3C};
    for (const uint8_t value : scmr_values)
        fx.cpu_write(0x303A, value);
    test_require(fx.state_.plot_bpp == 2 && fx.state_.screen_height == 3 &&
                     fx.state_.gsu_ram_access && fx.state_.gsu_rom_access,
                 "SCMR decode is wrong");

    fx.state_.cache_base = 0;
    fx.cpu_write(0x310F, 0xC3);
    test_require(fx.cache_[0x0F] == 0xC3 && fx.cache_valid_[0],
                 "cache-window write/final-byte valid marking is wrong");

    // Running-state gate: SCMR and SFR remain writable, unrelated registers do not.
    fx.state_.flags.running = true;
    const uint8_t old_screen_base = fx.state_.screen_base;
    fx.cpu_write(0x3038, 0xEE);
    test_require(fx.state_.screen_base == old_screen_base,
                 "running-state write gate allowed SCBR");
    fx.cpu_write(0x303A, 0x18);
    test_require(fx.state_.gsu_ram_access && fx.state_.gsu_rom_access,
                 "running-state write gate blocked SCMR");

    // Stopping via SFR invalidates cache state.
    fx.state_.cache_base = 0x1230;
    fx.cache_valid_[0] = true;
    fx.cpu_write(0x3030, 0x00);
    test_require(!fx.state_.flags.running && fx.state_.cache_base == 0 && !fx.cache_valid_[0],
                 "SFR stop did not clear cache state");
}

static void test_fx3_running_read_gate() {
    TestMemory memory{};
    SuperFx fx;
    fx.init(fx3_config, make_test_backend(memory));

    fx.state_.r[15] = 0xBEEF;
    fx.state_.flags.running = true;
    test_require(fx.cpu_read(0x301E) == 0xEF && fx.cpu_read(0x301F) == 0xBE,
                 "FX3 running R15 polling is wrong");
    test_require(fx.cpu_read(0x3000) == 0, "running FX3 exposed an ordinary register");
    test_require(fx.cpu_read(0x303B) == 0x52, "FX3 VCR value is wrong");
}

static void test_backend_callbacks() {
    std::array<std::atomic<uint8_t>, 64> ram{};
    std::array<uint8_t, 3u * 1024u * 1024u> rom{};

    rom[0] = 0x11;
    rom[0x200000] = 0x22;

    Rp2350FxBackendContext context {
        rom.data(), static_cast<uint32_t>(rom.size()),
        static_cast<uint32_t>(ram.size()),
        ram.data(), fx3_qspi_rom_read, backend_irq_write
    };

    FxBackend backend = fx_backend_create(&context);
    test_require(backend.rom_read(backend.context, 0x400000) == 0x11,
                 "backend ROM callback did not use FX3 logical mapper");
    test_require(backend.rom_read(backend.context, 0x600000) == 0x22,
                 "backend ROM callback did not map high FX3 ROM banks");

    backend.ram_write(backend.context, 10, 0xA5);
    test_require(backend.ram_read(backend.context, 10) == 0xA5,
                 "backend atomic RAM callbacks are wrong");
    test_require(backend.ram_read(backend.context, 1000) == 0xFF,
                 "backend out-of-range RAM read is wrong");
    backend.ram_write(backend.context, 1000, 0x55);

    backend_irq_level = false;
    backend.set_irq(backend.context, true);
    test_require(backend_irq_level, "backend IRQ callback was not forwarded");

    context.rom_read = nullptr;
    test_require(backend.rom_read(backend.context, 0x400000) == 0xFF,
                 "backend null ROM mapper fallback is wrong");
    context.rom_read = fx3_qspi_rom_read;

    context.ram = nullptr;
    test_require(backend.ram_read(backend.context, 0) == 0xFF,
                 "backend null RAM fallback is wrong");
    backend.ram_write(backend.context, 0, 0x12);
    context.ram = ram.data();

    context.irq_write = nullptr;
    backend.set_irq(backend.context, true);

    test_require(fx3_qspi_rom_read(nullptr, 0) == 0xFF,
                 "FX3 ROM mapper null-context fallback is wrong");
    context.rom = nullptr;
    test_require(fx3_qspi_rom_read(&context, 0) == 0xFF,
                 "FX3 ROM mapper null-image fallback is wrong");
    context.rom = rom.data();
    context.rom_size = 1;
    test_require(fx3_qspi_rom_read(&context, 0x400001) == 0xFF,
                 "FX3 ROM mapper size check is wrong");

    // A host linker address cannot live in the RP2350 XIP window, so host tests can
    // exercise the overlap-rejection branch but not the successful XIP pointer setup.
    context.rom_size = static_cast<uint32_t>(rom.size());
    test_require(!fx3_qspi_rom_init(context),
                 "host QSPI init should reject the host linker address as overlapping XIP");
}

int main() {
    test_register_reads_and_writes();
    test_fx3_running_read_gate();
    test_backend_callbacks();
    std::puts("register_backend_tests: PASS");
    return 0;
}
