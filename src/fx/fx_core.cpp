/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include <cstring>

#include "fx_core.h"
#include "pico/platform/sections.h"

void SuperFx::init(const FxConfig& config, const FxBackend& backend) {
    config_ = config;
    backend_ = backend;

    reset();
}

// Mesen-derived: closely follows MesenCE Gsu::Reset(), with local timing/backend reset state.
void SuperFx::reset() {
    std::memset(&state_, 0, sizeof(state_));
    std::memset(cache_, 0, sizeof(cache_));
    std::memset(cache_valid_, 0, sizeof(cache_valid_));

    state_.src_reg = 0;
    state_.dst_reg = 0;

    // FX3 powers up with SCBR=$40. This places the default planar screen buffer
    // at offset $10000 in the shared 128 KiB FX SRAM.
    if (config_.chip == FxChip::FX3)
        state_.screen_base = 0x40;

    // Initial GSU pipeline state.
    state_.program_read_buffer = 0x01;

    r15_changed_ = false;

    wait_for_rom_access_ = false;
    wait_for_ram_access_ = false;

    stopped_ = true;

    timing_initialized_ = false;
    last_master_clock_ = 0;
    target_cycles_ = 0;

    if (backend_.set_irq) backend_.set_irq(backend_.context, false);
}

void SuperFx::execute() {
    // The GSU is a one-byte prefetch pipeline: execute the old buffer while
    // read_opcode() fills it with the byte currently addressed by R15.
    const uint8_t opcode = read_opcode();

    execute_opcode(opcode);

    // Branches, jumps, and explicit R15 writes already supplied the next PC.
    if (r15_changed_)
        r15_changed_ = false;
    else
        state_.r[15]++;
}

// Mesen-derived: closely follows MesenCE Gsu::ReadOpCode().
uint8_t SuperFx::read_opcode() {
    const uint8_t result = state_.program_read_buffer;

    state_.program_read_buffer = read_program_byte();

    return result;
}

// Mesen-derived: closely follows MesenCE Gsu::ReadOperand().
uint8_t SuperFx::read_operand() {
    const uint8_t result = state_.program_read_buffer;

    state_.r[15]++;
    state_.program_read_buffer = read_program_byte();

    return result;
}

uint16_t SuperFx::read_src() const {
    return state_.r[state_.src_reg];
}

// Mesen-derived: closely follows MesenCE Gsu::WriteDestReg().
void SuperFx::write_dst(uint16_t value) {
    write_reg(state_.dst_reg, value);
}

// Mesen-derived: closely follows MesenCE Gsu::WriteRegister(), including R14/R15 side effects.
void SuperFx::write_reg(uint8_t reg, uint16_t value) {
    reg &= 0x0F;
    state_.r[reg] = value;

    if (reg == 14) {
        // Writing R14 initiates a buffered ROM read.
        state_.flags.rom_read_pending = true;
        state_.rom_delay = state_.clock_select ? 5 : 6;
    } else if (reg == 15) {
        // Suppress normal end-of-instruction PC increment.
        r15_changed_ = true;
    }
}

// Mesen-derived: closely follows MesenCE Gsu::ResetFlags().
void SuperFx::reset_prefix() {
    state_.flags.prefix = false;
    state_.flags.alt1 = false;
    state_.flags.alt2 = false;

    state_.src_reg = 0;
    state_.dst_reg = 0;
}

// Mesen-derived: closely follows MesenCE Gsu::InvalidateCache().
void __not_in_flash_func(SuperFx::invalidate_cache)() {
    std::memset(cache_valid_, 0, sizeof(cache_valid_));
}

const FxConfig fx1_config {
    FxChip::GSU1, FxTiming::Accurate, 0x5F
};

const FxConfig fx2_config {
    FxChip::GSU2, FxTiming::Accurate, 0x5F
};

// TODO: Exact hardware timing equivalence.
// FX3 Technical Specifications v1.0 identifies a 150 MHz RP2350B but does not
// define a GSU instruction-cycle cadence. Unlimited execution is therefore still
// an explicit firmware policy, not a primary-source timing model. MesenCE's ~4x
// multiplier is useful, but it does not upgrade this to a hardware guarantee.
const FxConfig fx3_config {
    FxChip::FX3, FxTiming::Unlimited, 0x6F
};

void SuperFx::run_accurate(uint32_t snes_master_clock) {
    if (!timing_initialized_) {
        // First sample establishes the clock-domain baseline - it does not
        // retroactively execute cycles that happened before initialization.
        last_master_clock_ = snes_master_clock;
        target_cycles_ = state_.cycles;
        timing_initialized_ = true;
        return;
    }

    const uint32_t elapsed = snes_master_clock - last_master_clock_;
    last_master_clock_ = snes_master_clock;
    target_cycles_ += elapsed;

    // Run until the real hardware timeline catches us.
    while (!stopped_ && state_.cycles < target_cycles_) {
        execute();
    }

    // If halted/stalled, time still passes.
    // ROM/RAM operations continue advancing.
    if (state_.cycles < target_cycles_) {
        const uint64_t remaining = target_cycles_ - state_.cycles;
        step(static_cast<uint32_t>(remaining));
    }
}

void SuperFx::run_unlimited(uint32_t instruction_budget) {
    // FX3 uses this path so completion time is limited by the RP2350 rather
    // than by the original GSU clock divider.
    while (!stopped_ && instruction_budget--) {
        execute();
    }
}

void SuperFx::run(uint32_t snes_master_clock, uint32_t unlimited_budget) {
    if (config_.timing == FxTiming::Accurate) {
        run_accurate(snes_master_clock);
    } else {
        run_unlimited(unlimited_budget);
    }
}
