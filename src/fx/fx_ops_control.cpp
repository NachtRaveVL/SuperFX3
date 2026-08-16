/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"

// $00: STOP
// Mesen-derived: closely follows MesenCE Gsu::STOP().
void SuperFx::op_stop() {
    if (config_.chip == FxChip::FX3) {
        // FX3 software polls R15 for completion.
        write_reg(15, 0);
    }

    // FX3 Technical Specifications v1.0 explicitly separates hardware from
    // emulation here: hardware has no FX completion IRQ, STOP sets R15 to zero,
    // and 65816 software polls R15. MesenCE intentionally keeps STOP IRQ support
    // for emulation, so the hardware firmware must diverge from Mesen on this point.
    if (!state_.irq_disabled && config_.chip != FxChip::FX3) {
        state_.flags.irq = true;
        if (backend_.set_irq) backend_.set_irq(backend_.context, true);
    }

    // Next start begins with synthetic NOP again.
    state_.program_read_buffer = 0x01;
    state_.flags.running = false;

    reset_prefix();

    update_running_state();
}

// $01: NOP
// Mesen-derived: closely follows MesenCE Gsu::NOP().
void SuperFx::op_nop() {
    reset_prefix();
}

// $02: CACHE
// Mesen-derived: closely follows MesenCE Gsu::CACHE().
void SuperFx::op_cache() {
    const uint16_t new_base = state_.r[15] & 0xFFF0;

    if (state_.cache_base != new_base) {
        state_.cache_base = new_base;
        invalidate_cache();
    }

    reset_prefix();
}

// $03: LSR
// Mesen-derived: closely follows MesenCE Gsu::LSR().
void SuperFx::op_lsr() {
    const uint16_t src = read_src();

    state_.flags.carry = (src & 0x0001) != 0;
    const uint16_t result = src >> 1;

    write_dst(result);

    state_.flags.zero = result == 0;
    state_.flags.sign = (result & 0x8000) != 0;

    reset_prefix();
}

// $04: ROL
// Mesen-derived: closely follows MesenCE Gsu::ROL().
void SuperFx::op_rol() {
    const uint16_t src = read_src();
    const uint16_t result = static_cast<uint16_t>((src << 1) | (state_.flags.carry ? 1 : 0));

    state_.flags.carry = (src & 0x8000) != 0;

    write_dst(result);

    state_.flags.zero = result == 0;
    state_.flags.sign = (result & 0x8000) != 0;

    reset_prefix();
}

// $05-$0F: BRANCH
// Mesen-derived: closely follows MesenCE Gsu::Branch().
void SuperFx::op_branch(bool condition) {
    const int8_t offset = static_cast<int8_t>(read_operand());

    if (condition)
        write_reg(15, static_cast<uint16_t>(state_.r[15] + offset));
}

// $10-$1F: TO / MOVE
// Mesen-derived: closely follows MesenCE Gsu::TO().
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

// $20-$2F: WITH
// Mesen-derived: closely follows MesenCE Gsu::WITH().
void SuperFx::op_with(uint8_t reg) {
    reg &= 0x0F;
    state_.src_reg = reg;
    state_.dst_reg = reg;

    state_.flags.prefix = true;
}

// $30-$3B: STORE
// ALT1 changes word store into byte store.
// Mesen-derived: closely follows MesenCE Gsu::STORE().
void SuperFx::op_store(uint8_t reg) {
    reg &= 0x0F;
    state_.ram_address = state_.r[reg];

    const uint16_t value = read_src();
    write_ram(state_.ram_address, static_cast<uint8_t>(value));

    if (!state_.flags.alt1)
        write_ram(state_.ram_address ^ 1, static_cast<uint8_t>(value >> 8));

    reset_prefix();
}

// $3C: LOOP
// Mesen-derived: closely follows MesenCE Gsu::LOOP().
void SuperFx::op_loop() {
    state_.r[12]--;
    state_.flags.zero = state_.r[12] == 0;
    state_.flags.sign = (state_.r[12] & 0x8000) != 0;

    if (!state_.flags.zero)
        write_reg(15, state_.r[13]);

    reset_prefix();
}

// $3D: ALT1
// Mesen-derived: closely follows MesenCE Gsu::ALT1().
void SuperFx::op_alt1() {
    state_.flags.prefix = false;
    state_.flags.alt1 = true;
}

// $3E: ALT2
// Mesen-derived: closely follows MesenCE Gsu::ALT2().
void SuperFx::op_alt2() {
    state_.flags.prefix = false;
    state_.flags.alt2 = true;
}

// $3F: ALT3
// Mesen-derived: closely follows MesenCE Gsu::ALT3().
void SuperFx::op_alt3() {
    state_.flags.prefix = false;
    state_.flags.alt1 = true;
    state_.flags.alt2 = true;
}

// $40-$4B: LOAD / LOADB
// ALT1:
//   0 = word, 1 = byte
// Mesen-derived: closely follows MesenCE Gsu::LOAD().
void SuperFx::op_load(uint8_t reg) {
    reg &= 0x0F;
    state_.ram_address = state_.r[reg];

    uint16_t value = read_ram(state_.ram_address);
    if (!state_.flags.alt1)
        value |= static_cast<uint16_t>(read_ram(state_.ram_address ^ 0x0001) << 8);

    write_dst(value);

    reset_prefix();
}

// $4C: PLOT / RPIX
// Mesen-derived: closely follows MesenCE Gsu::PLOT/RPIX().
void SuperFx::op_plot_rpix() {
    if (state_.flags.alt1) {
        // RPIX
        const uint8_t value = read_pixel(static_cast<uint8_t>(state_.r[1]),
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

// $4D: SWAP
// Mesen-derived: closely follows MesenCE Gsu::SWAP().
void SuperFx::op_swap() {
    const uint16_t src = read_src();
    const uint16_t value = (src >> 8) | (src << 8);

    write_dst(value);

    state_.flags.zero = value == 0;
    state_.flags.sign = (value & 0x8000) != 0;

    reset_prefix();
}

// $4E: COLOR / CMODE
// Mesen-derived: closely follows MesenCE Gsu::COLOR/CMODE().
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

// $4F: NOT
// Mesen-derived: closely follows MesenCE Gsu::NOT().
void SuperFx::op_not() {
    const uint16_t value = ~read_src();

    write_dst(value);

    state_.flags.zero = value == 0;
    state_.flags.sign = (value & 0x8000) != 0;

    reset_prefix();
}
