/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"

// $A0-$AF: IBT / SMS / LMS
// Normal = IBT Rn,#byte
// ALT1   = LMS Rn,(short)
// ALT2   = SMS (short),Rn
// ALT3 also resolves to LMS because ALT1 is tested first.
// Mesen-derived: closely follows MesenCE Gsu::IbtSmsLms().
void SuperFx::op_ibt_sms_lms(uint8_t reg) {
    reg &= 0x0F;

    if (state_.flags.alt1) {
        // LMS Load word from RAM using short address.
        // Operand represents WORD address, so left shifted to BYTE address.
        state_.ram_address = static_cast<uint16_t>(read_operand() * 2u);

        const uint8_t lsb = read_ram(state_.ram_address);
        const uint8_t msb = read_ram(state_.ram_address | 0x0001);

        write_reg(reg, static_cast<uint16_t>(
            static_cast<uint16_t>(lsb) | (static_cast<uint16_t>(msb) << 8)));
    } else if (state_.flags.alt2) {
        // SMS Store word to RAM using WORD address, so left shifted to BYTE address.
        state_.ram_address = static_cast<uint16_t>(read_operand() * 2u);
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

// $B0-$BF: FROM Rn
// WITH/prefix + FROM becomes MOVES.
// Mesen-derived: closely follows MesenCE Gsu::FROM().
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
    } else // FROM
        state_.src_reg = reg;
}

// $C0: HIB Return high byte of source register.
// Mesen-derived: closely follows MesenCE Gsu::HIB().
void SuperFx::op_hib() {
    const uint8_t value = static_cast<uint8_t>(read_src() >> 8);

    write_dst(value);

    state_.flags.zero = value == 0;
    state_.flags.sign = (value & 0x80) != 0;

    reset_prefix();
}

// $C1-$CF: OR / XOR
// ALT1 = XOR
// ALT2 = immediate nibble
// Normal = OR
// Mesen-derived: closely follows MesenCE Gsu::OrXor().
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

// $D0-$DE: INC Rn
// Mesen-derived: closely follows MesenCE Gsu::INC().
void SuperFx::op_inc(uint8_t reg) {
    reg &= 0x0F;
    const uint16_t value = state_.r[reg] + 1;

    write_reg(reg, value);

    state_.flags.zero = state_.r[reg] == 0;
    state_.flags.sign = (state_.r[reg] & 0x8000) != 0;

    reset_prefix();
}

// $DF: GETC / RAMB / ROMB/
// ALT2=0         GETC
// ALT2=1 ALT1=0  RAMB
// ALT2=1 ALT1=1  ROMB
// Mesen-derived: closely follows MesenCE Gsu::GetcRambRomb().
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

// $E0-$EE: DEC Rn
// Mesen-derived: closely follows MesenCE Gsu::DEC().
void SuperFx::op_dec(uint8_t reg) {
    reg &= 0x0F;
    const uint16_t value = state_.r[reg] - 1;

    write_reg(reg, value);

    state_.flags.zero = state_.r[reg] == 0;
    state_.flags.sign = (state_.r[reg] & 0x8000) != 0;

    reset_prefix();
}

// $EF: GETB family
// --      GETB
// ALT1    GETBH
// ALT2    GETBL
// ALT3    GETBS
// Mesen-derived: closely follows MesenCE Gsu::GETB().
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
        value = static_cast<uint16_t>(
            (read_src() & 0x00FFu) | (static_cast<uint32_t>(rom_data) << 8));
    } else // GETB
        value = rom_data;

    write_dst(value);
    reset_prefix();
}

// $F0-$FF: IWT / LM / SM
// Normal = IWT Rn,#word
// ALT1   = LM  Rn,(address)
// ALT2   = SM  (address),Rn
// ALT3 also resolves to LM.
// Mesen-derived: closely follows MesenCE Gsu::IwtLmSm().
void SuperFx::op_iwt_lm_sm(uint8_t reg) {
    reg &= 0x0F;

    if (state_.flags.alt1) {
        // LM: Load word from full 16-bit RAM address.
        const uint8_t addr_lo = read_operand();
        const uint8_t addr_hi = read_operand();
        state_.ram_address = static_cast<uint16_t>(
            static_cast<uint16_t>(addr_lo) | (static_cast<uint16_t>(addr_hi) << 8));

        const uint8_t lsb = read_ram(state_.ram_address);
        const uint8_t msb = read_ram(state_.ram_address ^ 0x0001);
        write_reg(reg, static_cast<uint16_t>(
            static_cast<uint16_t>(lsb) | (static_cast<uint16_t>(msb) << 8)));
    } else if (state_.flags.alt2) {
        // SM: Store word to full 16-bit RAM address.
        const uint8_t addr_lo = read_operand();
        const uint8_t addr_hi = read_operand();
        state_.ram_address = static_cast<uint16_t>(
            static_cast<uint16_t>(addr_lo) | (static_cast<uint16_t>(addr_hi) << 8));

        const uint16_t value = state_.r[reg];
        write_ram(state_.ram_address, static_cast<uint8_t>(value));
        write_ram(state_.ram_address ^ 0x0001, static_cast<uint8_t>(value >> 8));
    } else {
        // IWT: Immediate 16-bit word.
        const uint8_t lsb = read_operand();
        const uint8_t msb = read_operand();

        write_reg(reg, static_cast<uint16_t>(
            static_cast<uint16_t>(lsb) | (static_cast<uint16_t>(msb) << 8)));
    }

    reset_prefix();
}
