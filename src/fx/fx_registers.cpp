/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"

void SuperFx::execute_opcode(uint8_t opcode) {
    const uint8_t reg = opcode & 0x0F;

    switch (opcode >> 4) {
        // $00-$0F: STOP / NOP / CACHE / shifts / branches
        case 0x0:
            switch (reg) {
                case 0x0:
                    op_stop();
                    break;

                case 0x1:
                    op_nop();
                    break;

                case 0x2:
                    op_cache();
                    break;

                case 0x3:
                    op_lsr();
                    break;

                case 0x4:
                    op_rol();
                    break;

                case 0x5: // BRA
                    op_branch(true);
                    break;

                case 0x6: // BGE
                    op_branch(state_.flags.sign == state_.flags.overflow);
                    break;

                case 0x7: // BLT
                    op_branch(state_.flags.sign != state_.flags.overflow);
                    break;

                case 0x8: // BNE
                    op_branch(!state_.flags.zero);
                    break;

                case 0x9: // BEQ
                    op_branch(state_.flags.zero);
                    break;

                case 0xA: // BPL
                    op_branch(!state_.flags.sign);
                    break;

                case 0xB: // BMI
                    op_branch(state_.flags.sign);
                    break;

                case 0xC: // BCC
                    op_branch(!state_.flags.carry);
                    break;

                case 0xD: // BCS
                    op_branch(state_.flags.carry);
                    break;

                case 0xE: // BVC
                    op_branch(!state_.flags.overflow);
                    break;

                case 0xF: // BVS
                    op_branch(state_.flags.overflow);
                    break;
            }
            break;

        // $10-$1F: TO Rn / MOVE Rn
        case 0x1:
            op_to(reg);
            break;

        // $20-$2F: WITH Rn
        case 0x2:
            op_with(reg);
            break;

        // $30-$3F: STW/STB / LOOP / ALT1 / ALT2 / ALT3
        case 0x3:
            switch (reg) {
                case 0xC:
                    op_loop();
                    break;

                case 0xD:
                    op_alt1();
                    break;

                case 0xE:
                    op_alt2();
                    break;

                case 0xF:
                    op_alt3();
                    break;

                default:
                    // $30-$3B: STW / STB
                    op_store(reg);
                    break;
            }
            break;

        // $40-$4F: LDW/LDB / PLOT/RPIX / SWAP / COLOR/CMODE / NOT
        case 0x4:
            switch (reg) {
                case 0xC:
                    op_plot_rpix();
                    break;

                case 0xD:
                    op_swap();
                    break;

                case 0xE:
                    op_color_cmode();
                    break;

                case 0xF:
                    op_not();
                    break;

                default:
                    // $40-$4B: LDW / LDB
                    op_load(reg);
                    break;
            }
            break;

        // $50-$5F: ADD / ADC / ADD #n
        case 0x5:
            op_add(reg);
            break;

        // $60-$6F: SUB / SBC / SUB #n / CMP
        case 0x6:
            op_sub_compare(reg);
            break;

        // $70-$7F: MERGE / AND / BIC
        case 0x7:
            if (reg == 0)
                op_merge();
            else
                op_and_bic(reg);
            break;

        // $80-$8F: MULT / UMULT
        case 0x8:
            op_mult(reg);
            break;

        // $90-$9F: SBK / LINK / SEX / ASR / ROR / JMP / LOB /
        //          FMULT/LMULT
        case 0x9:
            switch (reg) {
                case 0x0: // SBK
                    op_sbk();
                    break;

                case 0x1: // LINK #1
                case 0x2: // LINK #2
                case 0x3: // LINK #3
                case 0x4: // LINK #4
                    op_link(reg);
                    break;

                case 0x5: // SEX
                    op_sex();
                    break;

                case 0x6: // ASR
                    op_asr();
                    break;

                case 0x7: // ROR
                    op_ror();
                    break;

                case 0x8:
                case 0x9:
                case 0xA:
                case 0xB:
                case 0xC:
                case 0xD:
                    // $98-$9D: JMP Rn / LJMP Rn
                    op_jmp(reg);
                    break;

                case 0xE: // LOB
                    op_lob();
                    break;

                case 0xF: // FMULT / LMULT
                    op_fmult_lmult();
                    break;
            }
            break;

        // $A0-$AF: IBT / SMS / LMS
        case 0xA:
            op_ibt_sms_lms(reg);
            break;

        // $B0-$BF: FROM Rn / MOVES Rn
        case 0xB:
            op_from(reg);
            break;

        // $C0-$CF: HIB / OR / XOR
        case 0xC:
            if (reg == 0)
                op_hib();
            else
                op_or_xor(reg);
            break;

        // $D0-$DF: INC Rn / GETC / RAMB / ROMB
        case 0xD:
            if (reg == 0xF)
                op_getc_ramb_romb();
            else
                op_inc(reg);
            break;

        // $E0-$EF: DEC Rn / GETB
        case 0xE:
            if (reg == 0xF)
                op_getb();
            else
                op_dec(reg);
            break;

        // $F0-$FF: IWT / LM / SM
        case 0xF:
            op_iwt_lm_sm(reg);
            break;
    }
}

// -------------------------------------------------------------
// $00: STOP
// -------------------------------------------------------------
void SuperFx::op_stop() {
    if (config_.chip == FxChip::FX3) {
        // FX3 software polls R15 for completion.
        write_reg(15, 0);
    }

    if (!state_.irq_disabled && config_.chip != FxChip::FX3) {
        state_.flags.irq = true;
        if (bus_.set_irq) bus_.set_irq(bus_.context, true);
    }

    // Next start begins with synthetic NOP again.
    state_.program_read_buffer = 0x01;
    state_.flags.running = false;

    reset_prefix();

    update_running_state();
}

// -------------------------------------------------------------
// $01: NOP
// -------------------------------------------------------------
void SuperFx::op_nop() { reset_prefix(); }

// -------------------------------------------------------------
// $02: CACHE
// -------------------------------------------------------------
void SuperFx::op_cache() {
    const uint16_t new_base = state_.r[15] & 0xFFF0;

    if (state_.cache_base != new_base) {
        state_.cache_base = new_base;
        invalidate_cache();
    }

    reset_prefix();
}

// -------------------------------------------------------------
// $03: LSR
// -------------------------------------------------------------
void SuperFx::op_lsr() {
    const uint16_t src = read_src();

    state_.flags.carry = (src & 0x0001) != 0;
    const uint16_t result = src >> 1;

    write_dst(result);

    state_.flags.zero = result == 0;
    state_.flags.sign = (result & 0x8000) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $04: ROL
// -------------------------------------------------------------
void SuperFx::op_rol() {
    const uint16_t src = read_src();
    const uint16_t result = static_cast<uint16_t>((src << 1) | (state_.flags.carry ? 1 : 0));

    state_.flags.carry = (src & 0x8000) != 0;

    write_dst(result);

    state_.flags.zero = result == 0;
    state_.flags.sign = (result & 0x8000) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $05-$0F: BRANCH
// -------------------------------------------------------------
void SuperFx::op_branch(bool condition) {
    const int8_t offset = static_cast<int8_t>(read_operand());

    if (condition)
        write_reg(15, state_.r[15] + offset);
}

// -------------------------------------------------------------
// $10-$1F: TO / MOVE
// -------------------------------------------------------------
void SuperFx::op_to(uint8_t reg) {
    reg &= 0x0F;

    if (state_.flags.prefix) {
        // WITH + TO becomes MOVE.
        write_reg(reg, read_src());

        reset_prefix();
    } else {
        state_.dst_reg = reg;
    }
}

// -------------------------------------------------------------
// $20-$2F: WITH
// -------------------------------------------------------------
void SuperFx::op_with(uint8_t reg) {
    reg &= 0x0F;

    state_.src_reg = reg;
    state_.dst_reg = reg;

    state_.flags.prefix = true;
}

// -------------------------------------------------------------
// $30-$3B: STORE
// ALT1 changes word store into byte store.
// -------------------------------------------------------------
void SuperFx::op_store(uint8_t reg) {
    reg &= 0x0F;
    state_.ram_address = state_.r[reg];

    const uint16_t value = read_src();
    write_ram(state_.ram_address, static_cast<uint8_t>(value));

    if (!state_.flags.alt1)
        write_ram(state_.ram_address ^ 1, static_cast<uint8_t>(value >> 8));

    reset_prefix();
}

// -------------------------------------------------------------
// $3C: LOOP
// -------------------------------------------------------------
void SuperFx::op_loop() {
    state_.r[12]--;
    state_.flags.zero = state_.r[12] == 0;
    state_.flags.sign = (state_.r[12] & 0x8000) != 0;

    if (!state_.flags.zero)
        write_reg(15, state_.r[13]);

    reset_prefix();
}

// -------------------------------------------------------------
// $3D: ALT1
// -------------------------------------------------------------
void SuperFx::op_alt1() {
    state_.flags.prefix = false;
    state_.flags.alt1 = true;
}

// -------------------------------------------------------------
// $3E: ALT2
// -------------------------------------------------------------
void SuperFx::op_alt2() {
    state_.flags.prefix = false;
    state_.flags.alt2 = true;
}

// -------------------------------------------------------------
// $3F: ALT3
// -------------------------------------------------------------
void SuperFx::op_alt3() {
    state_.flags.prefix = false;
    state_.flags.alt1 = true;
    state_.flags.alt2 = true;
}

// -------------------------------------------------------------
// $40-$4B: LOAD / LOADB
// ALT1: 0 = word, 1 = byte
// -------------------------------------------------------------
void SuperFx::op_load(uint8_t reg) {
    reg &= 0x0F;
    state_.ram_address = state_.r[reg];

    uint16_t value = read_ram(state_.ram_address);
    if (!state_.flags.alt1)
        value |= static_cast<uint16_t>(read_ram(state_.ram_address ^ 0x0001) << 8);

    write_dst(value);

    reset_prefix();
}

// -------------------------------------------------------------
// $4C: PLOT / RPIX
// -------------------------------------------------------------
void SuperFx::op_plot_rpix() {
    if (state_.flags.alt1) {
        // RPIX
        const uint8_t value = read_pixel(static_cast<uint8_t>(tate_.r[1]),
                                         static_cast<uint8_t>(state_.r[2]));
        state_.flags.zero = value == 0;

        // Mesen operates on an 8-bit pixel value here.
        state_.flags.sign = false;
        write_dst(value);
    } else {
        // PLOT
        draw_pixel(static_cast<uint8_t>(state_.r[1]),
                   static_cast<uint8_t>(state_.r[2]));
        state_.r[1]++;
    }

    reset_prefix();
}

// -------------------------------------------------------------
// $4D: SWAP
// -------------------------------------------------------------
void SuperFx::op_swap() {
    const uint16_t src = read_src();
    const uint16_t value = (src >> 8) | (src << 8);

    write_dst(value);

    state_.flags.zero = value == 0;
    state_.flags.sign = (value & 0x8000) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $4E: COLOR / CMODE
// -------------------------------------------------------------
void SuperFx::op_color_cmode() {
    if (state_.flags.alt1) {
        // CMODE
        const uint8_t value = static_cast<uint8_t>(read_src());
        state_.plot_transparent = (value & 0x01) != 0;
        state_.plot_dither = (value & 0x02) != 0;
        state_.color_high_nibble = (value & 0x04) != 0;
        state_.color_freeze_high = (value & 0x08) != 0;
        state_.object_mode = (value & 0x10) != 0;
    } else {
        // COLOR
        state_.color = get_color(static_cast<uint8_t>(read_src()));
    }

    reset_prefix();
}

// -------------------------------------------------------------
// $4F: NOT
// -------------------------------------------------------------
void SuperFx::op_not() {
    const uint16_t value = ~read_src();

    write_dst(value);

    state_.flags.zero = value == 0;
    state_.flags.sign = (value & 0x8000) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $50-$5F: ADD / ADC
// ALT2 = immediate nibble
// ALT1 = include carry
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// $60-$6F: SUB-CMP
// ALT1 ALT2
//   0    0     SUB Rn
//   1    0     SBC Rn
//   0    1     SUB #n
//   1    1     CMP Rn
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// $71-$7F: BIC / AND
// ALT1 = BIC
// ALT2 = immediate
// -------------------------------------------------------------
void SuperFx::op_and_bic(uint8_t reg) {
    const uint16_t operand = state_.flags.alt2 ? reg : state_.r[reg];
    uint16_t value = state_.flags.alt1 ? read_src() & ~operand // BIC
                                       : read_src() & operand; // AND

    write_dst(value);

    state_.flags.zero = value == 0;
    state_.flags.sign = (value & 0x8000) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $80-$8F: MULT
// Base = signed 8x8
// ALT1 = unsigned 8x8
// ALT2 = immediate nibble operand
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// $90: SBK
// Store word using last RAM address.
// -------------------------------------------------------------
void SuperFx::op_sbk() {
    const uint16_t value = read_src();

    write_ram(state_.ram_address, static_cast<uint8_t>(value));
    write_ram(state_.ram_address ^ 1, static_cast<uint8_t>(value >> 8));

    reset_prefix();
}

// -------------------------------------------------------------
// $91-$94: LINK
// -------------------------------------------------------------
void SuperFx::op_link(uint8_t value) {
    state_.r[11] = state_.r[15] + value;

    reset_prefix();
}

// -------------------------------------------------------------
// $95: SEX Sign-extend low 8 bits.
// -------------------------------------------------------------
void SuperFx::op_sex() {
    const int16_t value = static_cast<int8_t>(read_src());

    write_dst(static_cast<uint16_t>(value));

    state_.flags.zero = value == 0;
    state_.flags.sign = value < 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $96: ASR / DIV2
// ALT1 modifies rounding behavior.
// -------------------------------------------------------------
void SuperFx::op_asr() {
    const uint16_t src = read_src();
    state_.flags.carry = (src & 0x0001) != 0;

    uint16_t result = static_cast<uint16_t>(static_cast<int16_t>(src) >> 1);
    if (state_.flags.alt1)
        result += (static_cast<uint32_t>(src) + 1) >> 16;

    write_dst(result);

    state_.flags.zero = result == 0;
    state_.flags.sign = (result & 0x8000) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $97: ROR
// -------------------------------------------------------------
void SuperFx::op_ror() {
    const uint16_t src = read_src();
    const uint16_t result = (src >> 1) | (state_.flags.carry ? 0x8000 : 0x0000);
    state_.flags.carry = (src & 0x0001) != 0;

    write_dst(result);

    state_.flags.zero = result == 0;
    state_.flags.sign = (result & 0x8000) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $98-$9D: JMP R8-R13
// ALT1 = LJMP
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// $9E: LOB
// -------------------------------------------------------------
void SuperFx::op_lob() {
    const uint8_t value = static_cast<uint8_t>(read_src());

    write_dst(value);

    state_.flags.zero = value == 0;
    state_.flags.sign = (value & 0x80) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $9F: FMULT / LMULT
// signed Rsrc * R6
// FMULT:
//     high 16 -> destination
// LMULT (ALT1):
//     low 16  -> R4
//     high 16 -> destination
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// $A0-$AF: IBT / SMS / LMS
// Normal = IBT Rn,#byte
// ALT1   = LMS Rn,(short)
// ALT2   = SMS (short),Rn
// ALT3 also resolves to LMS because ALT1 is tested first.
// -------------------------------------------------------------
void SuperFx::op_ibt_sms_lms(uint8_t reg) {
    reg &= 0x0F;

    if (state_.flags.alt1) {
        // LMS Load word from RAM using short address.
        // Operand represents WORD address, so left shifted.
        state_.ram_address = static_cast<uint16_t>(read_operand()) << 1;

        const uint8_t lsb = read_ram(state_.ram_address);
        const uint8_t msb = read_ram(state_.ram_address | 0x0001);

        write_reg(reg, static_cast<uint16_t>(lsb) | (static_cast<uint16_t>(msb) << 8));
    } else if (state_.flags.alt2) {
        // SMS Store word to RAM using short address.
        state_.ram_address = static_cast<uint16_t>(read_operand()) << 1;
        const uint16_t value = state_.r[reg];

        write_ram(state_.ram_address, static_cast<uint8_t>(value));
        write_ram(state_.ram_address | 0x0001, static_cast<uint8_t>(value >> 8));
    } else {
        // IBT Immediate byte, SIGN EXTENDED.
        const int8_t value = static_cast<int8_t>(read_operand());
        write_reg(reg, static_cast<uint16_t>(static_cast<int16_t>(value)));
    }

    reset_prefix();
}

// -------------------------------------------------------------
// $B0-$BF: FROM Rn
// WITH/prefix + FROM becomes MOVES.
// -------------------------------------------------------------
void SuperFx::op_from(uint8_t reg) {
    reg &= 0x0F;

    if (state_.flags.prefix) { // MOVES
        const uint16_t value = state_.r[reg];

        write_dst(value);

        // Mesen uses bit 7 for Overflow here, not bit 15.
        state_.flags.overflow = (value & 0x0080) != 0;
        state_.flags.sign = (value & 0x8000) != 0;
        state_.flags.zero = value == 0;

        reset_prefix();
    } else { // FROM
        state_.src_reg = reg;
    }
}

// -------------------------------------------------------------
// $C0: HIB Return high byte of source register.
// -------------------------------------------------------------
void SuperFx::op_hib() {
    const uint8_t value = static_cast<uint8_t>(read_src() >> 8);

    write_dst(value);

    state_.flags.zero = value == 0;
    state_.flags.sign = (value & 0x80) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $C1-$CF: OR / XOR
// ALT1 = XOR
// ALT2 = immediate nibble
// Normal = OR
// -------------------------------------------------------------
void SuperFx::op_or_xor(uint8_t reg) {
    reg &= 0x0F;
    const uint16_t operand = state_.flags.alt2 ? reg : state_.r[reg];
    uint16_t value = state_.flags.alt1 ? read_src() ^ operand // XOR
                                       : read_src() | operand; // OR

    write_dst(value);

    state_.flags.zero = value == 0;
    state_.flags.sign = (value & 0x8000) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $D0-$DE: INC Rn
// -------------------------------------------------------------
void SuperFx::op_inc(uint8_t reg) {
    reg &= 0x0F;
    const uint16_t value = state_.r[reg] + 1;

    write_reg(reg, value);

    state_.flags.zero = state_.r[reg] == 0;
    state_.flags.sign = (state_.r[reg] & 0x8000) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $DF: GETC / RAMB / ROMB/
// ALT2=0         GETC
// ALT2=1 ALT1=0  RAMB
// ALT2=1 ALT1=1  ROMB
// -------------------------------------------------------------
void SuperFx::op_getc_ramb_romb() {
    if (!state_.flags.alt2) {
        // GETC: ROM buffer -> COLOR
        state_.color = get_color(read_rom_buffer());
    } else if (!state_.flags.alt1) {
        // RAMB: Select RAM bank.
        // Must finish old RAM operation first.
        wait_ram_operation();
        state_.ram_bank = read_src() & 0x01;
    } else {
        // ROMB: Select ROM data bank.
        // Must finish old ROM operation first.
        wait_rom_operation();
        state_.rom_bank = read_src() & 0x7F;
    }

    reset_prefix();
}

// -------------------------------------------------------------
// $E0-$EE: DEC Rn
// -------------------------------------------------------------
void SuperFx::op_dec(uint8_t reg) {
    reg &= 0x0F;
    const uint16_t value = state_.r[reg] - 1;

    write_reg(reg, value);

    state_.flags.zero = state_.r[reg] == 0;
    state_.flags.sign = (state_.r[reg] & 0x8000) != 0;

    reset_prefix();
}

// -------------------------------------------------------------
// $EF: GETB family
// --      GETB
// ALT1    GETBH
// ALT2    GETBL
// ALT3    GETBS
// -------------------------------------------------------------
void SuperFx::op_getb() {
    const uint8_t rom_data = read_rom_buffer();
    uint16_t value;

    if (state_.flags.alt1 && state_.flags.alt2) {
        // GETBS: Sign-extend ROM byte.
        value = static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(rom_data)));
    } else if (state_.flags.alt2) {
        // GETBL: Replace LOW byte of source.
        value = (read_src() & 0xFF00) | rom_data;
    } else if (state_.flags.alt1) {
        // GETBH: Replace HIGH byte of source.
        value = (read_src() & 0x00FF) | (static_cast<uint16_t>(rom_data) << 8);
    } else { // GETB
        value = rom_data;
    }

    write_dst(value);
    reset_prefix();
}

// -------------------------------------------------------------
// $F0-$FF: IWT / LM / SM
// Normal = IWT Rn,#word
// ALT1   = LM  Rn,(address)
// ALT2   = SM  (address),Rn
// ALT3 also resolves to LM.
// -------------------------------------------------------------
void SuperFx::op_iwt_lm_sm(uint8_t reg) {
    reg &= 0x0F;

    if (state_.flags.alt1) {
        // LM: Load word from full 16-bit RAM address.
        const uint8_t addr_lo = read_operand();
        const uint8_t addr_hi = read_operand();
        state_.ram_address = static_cast<uint16_t>(addr_lo) | (static_cast<uint16_t>(addr_hi) << 8);

        const uint8_t lsb = read_ram(state_.ram_address);
        const uint8_t msb = read_ram(state_.ram_address ^ 0x0001);
        write_reg(reg, static_cast<uint16_t>(lsb) | (static_cast<uint16_t>(msb) << 8));
    } else if (state_.flags.alt2) {
        // SM: Store word to full 16-bit RAM address.
        const uint8_t addr_lo = read_operand();
        const uint8_t addr_hi = read_operand();
        state_.ram_address = static_cast<uint16_t>(addr_lo) | (static_cast<uint16_t>(addr_hi) << 8);

        const uint16_t value = state_.r[reg];
        write_ram(state_.ram_address, static_cast<uint8_t>(value));
        write_ram(state_.ram_address ^ 0x0001, static_cast<uint8_t>(value >> 8));
    } else {
        // IWT: Immediate 16-bit word.
        const uint8_t lsb = read_operand();
        const uint8_t msb = read_operand();

        write_reg(reg, static_cast<uint16_t>(lsb) | (static_cast<uint16_t>(msb) << 8));
    }

    reset_prefix();
}

void SuperFx::unimplemented(uint8_t opcode) {
    static_cast<void>(opcode);
    state_.flags.running = false;
    update_running_state();
}
