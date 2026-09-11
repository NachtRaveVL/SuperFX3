#include <cstdint>
#include <cstdio>

#include "test_support.h"

struct ProgramResult {
    uint16_t r1;
    uint16_t r2;
    uint16_t r15;
    uint64_t cycles;
    uint8_t ram_lo;
    uint8_t ram_hi;
};

static ProgramResult run_store_then_stop(uint32_t budget) {
    TestMemory memory{};

    // IWT R1,#$1234
    // IWT R2,#$0100
    // FROM R1
    // STW (R2)
    // STOP
    const uint8_t program[] = {
        0xF1, 0x34, 0x12,
        0xF2, 0x00, 0x01,
        0xB1,
        0x32,
        0x00,
    };
    for (uint32_t i = 0; i < sizeof(program); ++i)
        memory.rom[i] = program[i];

    SuperFx fx;
    const FxBackend backend = make_test_backend(memory);
    fx.init(fx3_config, backend);

    // Drive the same CPU-visible setup sequence that firmware uses. The first
    // instruction is the synthetic reset NOP, then execution enters ROM at R15.
    fx.cpu_write(0x703A, 0x18);
    fx.cpu_write(0x701E, 0x00);
    fx.cpu_write(0x701F, 0x00);
    test_require(fx.running(), "public R15 start sequence did not start FX3");

    unsigned calls = 0;
    while (fx.running() && calls++ < 64)
        fx.run_unlimited(budget);

    test_require(!fx.running(), "FX3 program did not reach STOP");
    test_require(calls < 64, "FX3 program exceeded the architectural test call limit");
    test_require(fx.cpu_read(0x701E) == 0 && fx.cpu_read(0x701F) == 0,
                 "FX3 completion was not visible as R15=0 through the CPU interface");
    test_require(fx.cpu_ram_read(0x0100) == 0x34 && fx.cpu_ram_read(0x0101) == 0x12,
                 "STOP published completion before the final delayed word store committed");

    return ProgramResult{
        fx.state().r[1],
        fx.state().r[2],
        fx.state().r[15],
        fx.state().cycles,
        memory.ram[0x0100],
        memory.ram[0x0101],
    };
}

static void test_execution_chunking_is_architecturally_stable() {
    const ProgramResult one_at_a_time = run_store_then_stop(1);
    const ProgramResult bulk = run_store_then_stop(64);

    test_require(one_at_a_time.r1 == 0x1234 && one_at_a_time.r2 == 0x0100,
                 "end-to-end program produced the wrong register results");
    test_require(one_at_a_time.r15 == 0 && one_at_a_time.ram_lo == 0x34 && one_at_a_time.ram_hi == 0x12,
                 "end-to-end program produced the wrong completion state");
    test_require(one_at_a_time.r1 == bulk.r1 && one_at_a_time.r2 == bulk.r2 &&
                     one_at_a_time.r15 == bulk.r15 && one_at_a_time.cycles == bulk.cycles &&
                     one_at_a_time.ram_lo == bulk.ram_lo && one_at_a_time.ram_hi == bulk.ram_hi,
                 "FX3 result depends on how core 1 chunks run_unlimited calls");
}

static void test_cached_program_wrap_stays_in_bank() {
    for (uint8_t bank : {uint8_t{0x40}, uint8_t{0x70}, uint8_t{0x71}}) {
        TestMemory memory{};
        auto& bytes = bank == 0x40 ? memory.rom : memory.ram;
        const uint32_t base = bank == 0x71 ? 0x10000u : 0u;
        // Trampoline: IWT R0,#$FFF0; IBT R8,#bank; FROM R0; ALT1; LJMP R8; NOP.
        const uint8_t entry[] = {0xF0, 0xF0, 0xFF, 0xA8, bank, 0xB0, 0x3D, 0x98, 0x01};
        for (uint32_t i = 0; i < sizeof(entry); ++i)
            bytes[base + 0x0200u + i] = entry[i];
        for (uint32_t i = 0; i < 16; ++i)
            bytes[base + 0xFFF0u + i] = 0x01;
        bytes[base] = 0xA1; // IBT R1,#$2A, reached after 16-bit PC wrap.
        bytes[base + 1] = 0x2A;
        bytes[base + 2] = 0x00;

        SuperFx fx;
        fx.init(fx3_config, make_test_backend(memory));
        fx.cpu_write(0x7034, bank);
        fx.cpu_write(0x701E, 0x00);
        fx.cpu_write(0x701F, 0x02);
        fx.run_unlimited(64);
        test_require(!fx.running() && fx.state().r[1] == 0x2A,
                     "program-cache fill crossed the bank at 16-bit PC wrap");
    }
}

static void test_clear_retires_old_plot_data() {
    TestMemory memory{};
    // Draw partial rows in both pixel caches, clear third A, then RPIX flushes
    // any cache still pending. The cleared pixels must stay the clear color $81.
    const uint8_t program[] = {
        0xA0, 0x7F, 0x4E, // IBT R0,#$7F; COLOR
        0xA1, 0x00, 0xA2, 0x00, 0x4C, // PLOT (0,0)
        0xA1, 0x08, 0x4C, // PLOT (8,0)
        0xA0, 0x03, 0x70, // CLEAR A
        0xA1, 0x00, 0x13, 0x3D, 0x4C, // TO R3; ALT1; RPIX (0,0)
        0xA1, 0x08, 0x14, 0x3D, 0x4C, // TO R4; ALT1; RPIX (8,0)
        0x00,
    };
    for (uint32_t i = 0; i < sizeof(program); ++i)
        memory.rom[i] = program[i];
    SuperFx fx;
    fx.init(fx3_config, make_test_backend(memory));
    fx.cpu_write(0x703A, 0x07); // 8bpp, 160-line column stride.
    fx.cpu_write(0x701E, 0x00);
    fx.cpu_write(0x701F, 0x00);
    fx.run_unlimited(64);
    test_require(!fx.running() && fx.state().r[3] == 0x81 && fx.state().r[4] == 0x81,
                 "old pixel-cache data overwrote an FX3 clear");
}

int main() {
    test_execution_chunking_is_architecturally_stable();
    test_clear_retires_old_plot_data();
    test_cached_program_wrap_stays_in_bank();
    std::puts("architectural_tests: PASS");
    return 0;
}
