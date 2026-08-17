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

int main() {
    test_execution_chunking_is_architecturally_stable();
    std::puts("architectural_tests: PASS");
    return 0;
}
