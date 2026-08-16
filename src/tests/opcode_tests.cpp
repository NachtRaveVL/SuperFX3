#include <algorithm>
#include <cstdint>
#include <cstdio>

#define private public
#include "../fx/fx_core.h"
#undef private

#include "test_support.h"

static void prepare(SuperFx& fx, TestMemory& memory, const FxConfig& config = fx2_config) {
    std::fill(memory.ram.begin(), memory.ram.end(), 0);
    memory.irq = false;

    fx.init(config, make_test_backend(memory));
    fx.state_.flags.running = true;
    fx.state_.gsu_rom_access = true;
    fx.state_.gsu_ram_access = true;
    fx.state_.plot_bpp = 8;
    fx.state_.screen_height = 0;
    fx.state_.color = 0x5A;
    fx.state_.src_reg = 0;
    fx.state_.dst_reg = 1;
    fx.state_.program_bank = 0;
    fx.state_.rom_bank = 0;
    fx.state_.ram_bank = 0;
    fx.state_.program_read_buffer = 0x02;
    fx.state_.rom_read_buffer = 0x7F;
    fx.state_.r[0] = 0x1234;
    fx.state_.r[1] = 0x0010;
    fx.state_.r[2] = 0x0020;
    fx.state_.r[3] = 0x0030;
    fx.state_.r[4] = 0x0040;
    fx.state_.r[5] = 0x0050;
    fx.state_.r[6] = 0x0003;
    fx.state_.r[7] = 0xAA55;
    fx.state_.r[8] = 0xCC33;
    fx.state_.r[9] = 0x0090;
    fx.state_.r[10] = 0x00A0;
    fx.state_.r[11] = 0x00B0;
    fx.state_.r[12] = 2;
    fx.state_.r[13] = 0x0200;
    fx.state_.r[14] = 0x0100;
    fx.state_.r[15] = 0x0100;
}

static void test_every_opcode_dispatches() {
    TestMemory memory{};
    SuperFx fx;

    // This is deliberately a smoke sweep. The focused tests below assert semantics;
    // this loop makes sure every decoder entry reaches a live handler with sane state.
    for (uint32_t opcode = 0; opcode < 256; ++opcode) {
        prepare(fx, memory);
        fx.execute_opcode(static_cast<uint8_t>(opcode));
    }
}

static void test_control_variants() {
    TestMemory memory{};
    SuperFx fx;
    prepare(fx, memory);

    fx.state_.r[0] = 0x8001;
    fx.op_lsr();
    test_require(fx.state_.r[1] == 0x4000 && fx.state_.flags.carry,
                 "LSR result/carry is wrong");

    prepare(fx, memory);
    fx.state_.r[0] = 0x8000;
    fx.state_.flags.carry = true;
    fx.op_rol();
    test_require(fx.state_.r[1] == 0x0001 && fx.state_.flags.carry,
                 "ROL result/carry is wrong");

    prepare(fx, memory);
    fx.state_.program_read_buffer = 0xFE;
    const uint16_t branch_pc = fx.state_.r[15];
    fx.op_branch(true);
    test_require(fx.state_.r[15] == branch_pc - 1,
                 "taken branch did not apply signed operand after operand fetch");

    prepare(fx, memory);
    fx.state_.program_read_buffer = 0x7F;
    const uint16_t untaken_pc = fx.state_.r[15];
    fx.op_branch(false);
    test_require(fx.state_.r[15] == untaken_pc + 1,
                 "untaken branch did not consume exactly one operand byte");

    prepare(fx, memory);
    fx.op_with(3);
    fx.state_.r[3] = 0xBEEF;
    fx.op_to(5);
    test_require(fx.state_.r[5] == 0xBEEF && !fx.state_.flags.prefix,
                 "WITH+TO MOVE behavior is wrong");

    prepare(fx, memory);
    fx.state_.flags.alt1 = true;
    fx.state_.r[0] = 0xABCD;
    fx.state_.r[3] = 0x0100;
    fx.op_store(3);
    fx.wait_ram_operation();
    test_require(memory.ram[0x0100] == 0xCD && memory.ram[0x0101] == 0,
                 "byte STORE wrote more than the low byte");

    prepare(fx, memory);
    fx.state_.r[12] = 1;
    fx.state_.r[13] = 0x4444;
    fx.op_loop();
    test_require(fx.state_.r[12] == 0 && fx.state_.flags.zero,
                 "LOOP zero termination is wrong");

    prepare(fx, memory);
    memory.ram[0x100] = 0x78;
    memory.ram[0x101] = 0x56;
    fx.state_.r[3] = 0x0100;
    fx.op_load(3);
    test_require(fx.state_.r[1] == 0x5678, "word LOAD result is wrong");

    prepare(fx, memory);
    memory.ram[0x100] = 0x78;
    memory.ram[0x101] = 0x56;
    fx.state_.flags.alt1 = true;
    fx.state_.r[3] = 0x0100;
    fx.op_load(3);
    test_require(fx.state_.r[1] == 0x0078, "byte LOAD result is wrong");

    prepare(fx, memory);
    fx.state_.flags.alt1 = true;
    fx.state_.r[0] = 0x001F;
    fx.op_color_cmode();
    test_require(fx.state_.plot_transparent && fx.state_.plot_dither &&
                     fx.state_.color_high_nibble && fx.state_.color_freeze_high && fx.state_.object_mode,
                 "CMODE did not decode its control bits");
}

static void test_alu_variants() {
    TestMemory memory{};
    SuperFx fx;

    prepare(fx, memory);
    fx.state_.r[0] = 0xFFFF;
    fx.state_.r[2] = 1;
    fx.op_add(2);
    test_require(fx.state_.r[1] == 0 && fx.state_.flags.carry && fx.state_.flags.zero,
                 "ADD carry/zero behavior is wrong");

    prepare(fx, memory);
    fx.state_.r[0] = 1;
    fx.state_.flags.alt1 = true;
    fx.state_.flags.carry = false;
    fx.state_.r[2] = 1;
    fx.op_sub_compare(2);
    test_require(fx.state_.r[1] == 0xFFFF && !fx.state_.flags.carry,
                 "SBC borrow behavior is wrong");

    prepare(fx, memory);
    fx.state_.r[0] = 0xAAAA;
    fx.state_.r[2] = 0x0F0F;
    fx.state_.flags.alt1 = true;
    fx.op_and_bic(2);
    test_require(fx.state_.r[1] == static_cast<uint16_t>(0xAAAA & ~0x0F0F),
                 "BIC result is wrong");

    prepare(fx, memory);
    fx.state_.r[0] = 0x00FE;
    fx.state_.r[2] = 0x0002;
    fx.op_mult(2);
    test_require(fx.state_.r[1] == 0xFFFC,
                 "signed MULT result is wrong");

    prepare(fx, memory);
    fx.state_.r[0] = 0x00FE;
    fx.state_.r[2] = 0x0002;
    fx.state_.flags.alt1 = true;
    fx.op_mult(2);
    test_require(fx.state_.r[1] == 0x01FC,
                 "unsigned MULT result is wrong");

    prepare(fx, memory);
    fx.state_.r[0] = 0x0080;
    fx.op_sex();
    test_require(fx.state_.r[1] == 0xFF80 && fx.state_.flags.sign,
                 "SEX result is wrong");

    prepare(fx, memory);
    fx.state_.r[0] = 0x8001;
    fx.state_.flags.alt1 = true;
    fx.op_asr();
    test_require(fx.state_.flags.carry, "ASR did not preserve the shifted-out bit");

    prepare(fx, memory);
    fx.state_.r[0] = 0x0001;
    fx.state_.flags.carry = true;
    fx.op_ror();
    test_require(fx.state_.r[1] == 0x8000 && fx.state_.flags.carry,
                 "ROR result/carry is wrong");

    prepare(fx, memory);
    fx.state_.r[8] = 0x4321;
    fx.op_jmp(8);
    test_require(fx.state_.r[15] == 0x4321, "JMP target is wrong");

    prepare(fx, memory);
    fx.state_.flags.alt1 = true;
    fx.state_.r[8] = 0x0062;
    fx.state_.r[0] = 0x3456;
    fx.op_jmp(8);
    test_require(fx.state_.program_bank == 0x62 && fx.state_.r[15] == 0x3456,
                 "LJMP bank/target is wrong");

    prepare(fx, memory);
    fx.state_.r[0] = 0xFFFE;
    fx.state_.r[6] = 3;
    fx.state_.flags.alt1 = true;
    fx.op_fmult_lmult();
    test_require(fx.state_.r[4] == 0xFFFA && fx.state_.r[1] == 0xFFFF,
                 "LMULT low/high result is wrong");
}

static void test_data_variants() {
    TestMemory memory{};
    SuperFx fx;

    prepare(fx, memory);
    fx.state_.program_read_buffer = 0x80;
    fx.op_ibt_sms_lms(3);
    test_require(fx.state_.r[3] == 0xFF80, "IBT sign extension is wrong");

    prepare(fx, memory);
    fx.state_.flags.alt1 = true;
    fx.state_.program_read_buffer = 0x20;
    memory.ram[0x40] = 0x34;
    memory.ram[0x41] = 0x12;
    fx.op_ibt_sms_lms(3);
    test_require(fx.state_.r[3] == 0x1234, "LMS result is wrong");

    prepare(fx, memory);
    fx.state_.flags.alt2 = true;
    fx.state_.program_read_buffer = 0x20;
    fx.state_.r[3] = 0xBEEF;
    fx.op_ibt_sms_lms(3);
    fx.wait_ram_operation();
    test_require(memory.ram[0x40] == 0xEF && memory.ram[0x41] == 0xBE,
                 "SMS result is wrong");

    prepare(fx, memory);
    fx.op_with(1);
    fx.state_.r[2] = 0x8080;
    fx.op_from(2);
    test_require(fx.state_.r[1] == 0x8080 && fx.state_.flags.sign && fx.state_.flags.overflow,
                 "WITH+FROM MOVES behavior is wrong");

    prepare(fx, memory);
    fx.state_.r[0] = 0xABCD;
    fx.op_hib();
    test_require(fx.state_.r[1] == 0x00AB, "HIB result is wrong");

    prepare(fx, memory);
    fx.state_.r[0] = 0x1200;
    fx.state_.r[2] = 0x0034;
    fx.op_or_xor(2);
    test_require(fx.state_.r[1] == 0x1234, "OR result is wrong");

    prepare(fx, memory);
    fx.state_.flags.alt1 = true;
    fx.state_.r[0] = 0xFFFF;
    fx.state_.r[2] = 0x0F0F;
    fx.op_or_xor(2);
    test_require(fx.state_.r[1] == 0xF0F0, "XOR result is wrong");

    prepare(fx, memory);
    fx.state_.r[3] = 0xFFFF;
    fx.op_inc(3);
    test_require(fx.state_.r[3] == 0 && fx.state_.flags.zero, "INC wrap is wrong");
    fx.op_dec(3);
    test_require(fx.state_.r[3] == 0xFFFF && fx.state_.flags.sign, "DEC wrap is wrong");

    prepare(fx, memory);
    fx.state_.rom_read_buffer = 0xA5;
    fx.op_getc_ramb_romb();
    test_require(fx.state_.color == 0xA5, "GETC did not copy ROM buffer to color");

    prepare(fx, memory);
    fx.state_.flags.alt2 = true;
    fx.state_.r[0] = 1;
    fx.op_getc_ramb_romb();
    test_require(fx.state_.ram_bank == 1, "RAMB did not select RAM bank");

    prepare(fx, memory);
    fx.state_.flags.alt1 = true;
    fx.state_.flags.alt2 = true;
    fx.state_.r[0] = 0x0065;
    fx.op_getc_ramb_romb();
    test_require(fx.state_.rom_bank == 0x65, "ROMB did not select ROM bank");

    for (uint8_t mode = 0; mode < 4; ++mode) {
        prepare(fx, memory);
        fx.state_.rom_read_buffer = 0x80;
        fx.state_.r[0] = 0x1234;
        fx.state_.flags.alt1 = (mode & 1u) != 0;
        fx.state_.flags.alt2 = (mode & 2u) != 0;
        fx.op_getb();
    }

    prepare(fx, memory);
    fx.state_.program_read_buffer = 0x34;
    memory.rom[0x101] = 0x12;
    fx.op_iwt_lm_sm(3);
    test_require(fx.state_.r[3] == 0x1234, "IWT result is wrong");

    prepare(fx, memory);
    fx.state_.flags.alt1 = true;
    fx.state_.program_read_buffer = 0x40;
    memory.rom[0x101] = 0x00;
    memory.ram[0x40] = 0x78;
    memory.ram[0x41] = 0x56;
    fx.op_iwt_lm_sm(3);
    test_require(fx.state_.r[3] == 0x5678, "LM result is wrong");

    prepare(fx, memory);
    fx.state_.flags.alt2 = true;
    fx.state_.program_read_buffer = 0x40;
    memory.rom[0x101] = 0x00;
    fx.state_.r[3] = 0xCAFE;
    fx.op_iwt_lm_sm(3);
    fx.wait_ram_operation();
    test_require(memory.ram[0x40] == 0xFE && memory.ram[0x41] == 0xCA,
                 "SM result is wrong");
}

static void test_graphics_variants() {
    TestMemory memory{};
    SuperFx fx;
    prepare(fx, memory);

    fx.state_.color = 0xA0;
    fx.state_.color_high_nibble = true;
    test_require(fx.get_color(0xBC) == 0xAB, "high-nibble COLOR mode is wrong");
    fx.state_.color_high_nibble = false;
    fx.state_.color_freeze_high = true;
    test_require(fx.get_color(0xBC) == 0xAC, "freeze-high COLOR mode is wrong");

    for (uint8_t mode = 0; mode < 4; ++mode) {
        fx.state_.object_mode = mode == 3;
        fx.state_.screen_height = mode;
        (void)fx.get_tile_index(0x98, 0xA8);
    }

    fx.state_.plot_bpp = 2;
    fx.state_.color = 0;
    test_require(fx.is_transparent_pixel(), "2bpp transparent-color detection is wrong");
    fx.state_.plot_bpp = 4;
    test_require(fx.is_transparent_pixel(), "4bpp transparent-color detection is wrong");
    fx.state_.plot_bpp = 8;
    test_require(fx.is_transparent_pixel(), "8bpp transparent-color detection is wrong");
    fx.state_.plot_bpp = 3;
    test_require(fx.is_transparent_pixel(), "invalid-bpp transparency fallback is wrong");

    prepare(fx, memory);
    fx.state_.plot_bpp = 4;
    fx.state_.plot_transparent = true;
    fx.state_.plot_dither = true;
    fx.state_.color = 0xAB;
    fx.draw_pixel(1, 2);
    fx.draw_pixel(2, 2);
    fx.flush_primary_cache(8, 2);
    fx.flush_primary_cache(16, 2);

    prepare(fx, memory);
    fx.state_.plot_bpp = 2;
    fx.state_.plot_transparent = true;
    fx.state_.primary_cache = FxPixelCache{0, 0, {1, 2, 3, 0, 0, 0, 0, 0}, 0x07};
    memory.ram[0] = 0xF0;
    fx.write_pixel_cache(fx.state_.primary_cache);
    test_require(fx.state_.primary_cache.valid_bits == 0,
                 "partial plot-cache write did not clear valid bits");

    prepare(fx, memory);
    fx.state_.plot_bpp = 2;
    const uint32_t address = fx.get_tile_address(3, 4);
    memory.ram[address + 0] = 0x10;
    memory.ram[address + 1] = 0x10;
    (void)fx.read_pixel(3, 4);
}

static void test_memory_and_timing_paths() {
    TestMemory memory{};
    SuperFx fx;

    prepare(fx, memory, fx2_config);
    fx.state_.flags.running = true;
    fx.state_.gsu_rom_access = true;
    fx.state_.gsu_ram_access = true;
    test_require(!fx.rom_access_allowed() && !fx.ram_access_allowed(),
                 "legacy ownership gates should block CPU while GSU owns ROM/RAM");
    test_require(fx.cpu_rom_read(0x04) == 0x04 && fx.cpu_ram_read(0x10) == 0,
                 "legacy blocked CPU access behavior is wrong");
    memory.ram[0x10] = 0x33;
    fx.cpu_ram_write(0x10, 0x99);
    test_require(memory.ram[0x10] == 0x33, "blocked legacy RAM write reached backend");

    fx.state_.gsu_rom_access = false;
    fx.state_.gsu_ram_access = false;
    test_require(fx.rom_access_allowed() && fx.ram_access_allowed(),
                 "legacy ownership gates did not release CPU access");

    prepare(fx, memory, fx2_config);
    fx.state_.gsu_rom_access = false;
    fx.wait_for_rom_access();
    fx.state_.gsu_ram_access = false;
    fx.wait_for_ram_access();
    test_require(fx.stopped_, "GSU WAIT did not stop execution");

    prepare(fx, memory, fx2_config);
    fx.state_.program_bank = 0x70;
    fx.state_.cache_base = 0x8000;
    fx.state_.r[15] = 0x0100;
    memory.ram[0x0100] = 0xA5;
    test_require(fx.read_program_byte() == 0xA5, "RAM program-bank fetch is wrong");

    fx.state_.program_bank = 0x72;
    test_require(fx.read_program_byte() == 0, "unmapped program-bank fetch is wrong");

    prepare(fx, memory, fx2_config);
    fx.state_.program_bank = 0;
    fx.state_.cache_base = 0;
    fx.state_.r[15] = 0;
    memory.rom[0] = 0x77;
    test_require(fx.read_program_byte() == 0x77 && fx.cache_valid_[0],
                 "program-cache fill path is wrong");
    test_require(fx.read_program_byte() == 0x77,
                 "program-cache hit path is wrong");

    prepare(fx, memory, fx2_config);
    fx.state_.flags.running = true;
    fx.stopped_ = false;
    fx.state_.program_read_buffer = 0x00;
    fx.run_accurate(100);
    test_require(fx.timing_initialized_, "accurate timing did not initialize its baseline");
    fx.run_accurate(110);
    test_require(fx.state_.cycles >= 10, "accurate timing did not advance to target cycles");

    prepare(fx, memory, fx3_config);
    fx.state_.flags.running = true;
    fx.stopped_ = false;
    fx.state_.program_read_buffer = 0x01;
    fx.run(0, 1);
    test_require(fx.state_.r[15] == 0x0101, "unlimited run path did not execute its budget");
}

int main() {
    test_every_opcode_dispatches();
    test_control_variants();
    test_alu_variants();
    test_data_variants();
    test_graphics_variants();
    test_memory_and_timing_paths();
    std::puts("opcode_tests: PASS");
    return 0;
}
