/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"

// $50-$5F: ADD / ADC
// ALT2 = immediate nibble
// ALT1 = include carry
// Mesen-derived: closely follows MesenCE Gsu::Add().
void SuperFx::op_add(uint8_t reg) {
    const uint16_t src = read_src();
    const uint16_t operand = state_.flags.alt2 ? reg : state_.r[reg];
    uint32_t result = static_cast<uint32_t>(src) + operand;
    if (state_.flags.alt1) result += state_.flags.carry ? 1 : 0;

    const uint16_t result16 = static_cast<uint16_t>(result);
    state_.flags.carry = (result & 0x10000) != 0;
    state_.flags.overflow = ((~(src ^ operand) & (operand ^ result16) & 0x8000) != 0);
    state_.flags.sign = (result16 & 0x8000) != 0;
    state_.flags.zero = result16 == 0;

    write_dst(result16);

    reset_prefix();
}

// $60-$6F: SUB-CMP
// ALT1 ALT2
//   0    0     SUB Rn
//   1    0     SBC Rn
//   0    1     SUB #n
//   1    1     CMP Rn
// Mesen-derived: closely follows MesenCE Gsu::SubCompare().
void SuperFx::op_sub_compare(uint8_t reg) {
    const uint16_t src = read_src();

    uint16_t operand = state_.flags.alt2 && !state_.flags.alt1 ? reg : state_.r[reg];
    int32_t result = static_cast<int32_t>(src) - static_cast<int32_t>(operand);

    // SBC
    if (state_.flags.alt1 && !state_.flags.alt2 && !state_.flags.carry) result--;

    const uint16_t result16 = static_cast<uint16_t>(result);
    state_.flags.carry = result >= 0;
    state_.flags.overflow = (((src ^ operand) & (src ^ result16) & 0x8000) != 0);
    state_.flags.sign = (result16 & 0x8000) != 0;
    state_.flags.zero = result16 == 0;

    // ALT3 = CMP: flags only.
    if (!(state_.flags.alt1 && state_.flags.alt2)) write_dst(result16);

    reset_prefix();
}

// $71-$7F: BIC / AND
// ALT1 = BIC
// ALT2 = immediate
// Mesen-derived: closely follows MesenCE Gsu::AndBitClear().
void SuperFx::op_and_bic(uint8_t reg) {
    const uint16_t operand = state_.flags.alt2 ? reg : state_.r[reg];
    uint16_t value = state_.flags.alt1 ? read_src() & ~operand // BIC
                                       : read_src() & operand; // AND

    write_dst(value);

    state_.flags.zero = value == 0;
    state_.flags.sign = (value & 0x8000) != 0;

    reset_prefix();
}

// $80-$8F: MULT
// Base = signed 8x8
// ALT1 = unsigned 8x8
// ALT2 = immediate nibble operand
// Mesen-derived: closely follows MesenCE Gsu::MULT().
void SuperFx::op_mult(uint8_t reg) {
    const uint16_t operand = state_.flags.alt2 ? reg : state_.r[reg];
    uint16_t value = state_.flags.alt1 ? static_cast<uint16_t>(static_cast<uint8_t>(read_src()) * static_cast<uint8_t>(operand))
                                       : static_cast<uint16_t>(static_cast<int8_t>(read_src()) * static_cast<int8_t>(operand));

    write_dst(value);

    state_.flags.sign = (value & 0x8000) != 0;
    state_.flags.zero = value == 0;

    reset_prefix();

    // Mesen charges an additional multiplier delay.
    step(state_.high_speed ? 1 : 2);
}

// $90: SBK
// Store word using last RAM address.
// Mesen-derived: closely follows MesenCE Gsu::SBK().
void SuperFx::op_sbk() {
    const uint16_t value = read_src();

    write_ram(state_.ram_address, static_cast<uint8_t>(value));
    write_ram(state_.ram_address ^ 1, static_cast<uint8_t>(value >> 8));

    reset_prefix();
}

// $91-$94: LINK
// Mesen-derived: closely follows MesenCE Gsu::LINK().
void SuperFx::op_link(uint8_t value) {
    state_.r[11] = state_.r[15] + value;

    reset_prefix();
}

// $95: SEX Sign-extend low 8 bits.
// Mesen-derived: closely follows MesenCE Gsu::SignExtend().
void SuperFx::op_sex() {
    const int16_t value = static_cast<int8_t>(read_src());

    write_dst(static_cast<uint16_t>(value));

    state_.flags.zero = value == 0;
    state_.flags.sign = value < 0;

    reset_prefix();
}

// $96: ASR / DIV2
// ALT1 modifies rounding behavior.
// Mesen-derived: closely follows MesenCE Gsu::ASR().
void SuperFx::op_asr() {
    const uint16_t src = read_src();
    state_.flags.carry = (src & 0x0001) != 0;

    uint16_t result = static_cast<uint16_t>(static_cast<int16_t>(src) >> 1);
    if (state_.flags.alt1)
        result = static_cast<uint16_t>(result + ((static_cast<uint32_t>(src) + 1) >> 16));

    write_dst(result);

    state_.flags.zero = result == 0;
    state_.flags.sign = (result & 0x8000) != 0;

    reset_prefix();
}

// $97: ROR
// Mesen-derived: closely follows MesenCE Gsu::ROR().
void SuperFx::op_ror() {
    const uint16_t src = read_src();
    const uint16_t result = (src >> 1) | (state_.flags.carry ? 0x8000 : 0x0000);
    state_.flags.carry = (src & 0x0001) != 0;

    write_dst(result);

    state_.flags.zero = result == 0;
    state_.flags.sign = (result & 0x8000) != 0;

    reset_prefix();
}

// $98-$9D: JMP R8-R13
// ALT1 = LJMP
// Mesen-derived: closely follows MesenCE Gsu::JMP().
void SuperFx::op_jmp(uint8_t reg) {
    reg &= 0x0F;

    if (state_.flags.alt1) {
        // LJMP
        state_.program_bank = state_.r[reg] & 0x7F;

        write_reg(15, read_src());

        state_.cache_base = state_.r[15] & 0xFFF0;

        invalidate_cache();
    } else {
        // JMP
        write_reg(15, state_.r[reg]);
    }

    reset_prefix();
}

// $9E: LOB
// Mesen-derived: closely follows MesenCE Gsu::LOB().
void SuperFx::op_lob() {
    const uint8_t value = static_cast<uint8_t>(read_src());

    write_dst(value);

    state_.flags.zero = value == 0;
    state_.flags.sign = (value & 0x80) != 0;

    reset_prefix();
}

// $9F: FMULT / LMULT
// signed Rsrc * R6
// FMULT:
//     high 16 -> destination
// LMULT (ALT1):
//     low 16  -> R4
//     high 16 -> destination
// Mesen-derived: closely follows MesenCE Gsu::FMultLMult().
void SuperFx::op_fmult_lmult() {
    const int32_t lhs = static_cast<int16_t>(read_src());
    const int32_t rhs = static_cast<int16_t>(state_.r[6]);
    const uint32_t result = static_cast<uint32_t>(lhs * rhs);

    if (state_.flags.alt1)
        state_.r[4] = static_cast<uint16_t>(result);

    const uint16_t high = static_cast<uint16_t>(result >> 16);
    write_dst(high);

    state_.flags.carry = (result & 0x00008000u) != 0;
    state_.flags.sign = (high & 0x8000) != 0;
    state_.flags.zero = high == 0;

    reset_prefix();

    const uint32_t multiply_cycles = state_.high_speed ? 3 : 7;
    step(multiply_cycles * (state_.clock_select ? 1 : 2));
}
