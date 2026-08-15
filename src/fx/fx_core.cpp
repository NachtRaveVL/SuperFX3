/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include <cstring>

#include "fx_core.h"

void SuperFx::init(const FxConfig& config, const FxBus& bus) {
    config_ = config;
    bus_ = bus;

    reset();
}

void SuperFx::reset() {
    std::memset(&state_, 0, sizeof(state_));
    std::memset(cache_, 0, sizeof(cache_));
    std::memset(cache_valid_, 0, sizeof(cache_valid_));

    state_.src_reg = 0;
    state_.dst_reg = 0;

    // Initial GSU pipeline state.
    state_.program_read_buffer = 0x01;

    r15_changed_ = false;

    wait_for_rom_access_ = false;
    wait_for_ram_access_ = false;

    stopped_ = true;

    timing_initialized_ = false;
    last_master_clock_ = 0;
    target_cycles_ = 0;

    if (bus_.set_irq) bus_.set_irq(bus_.context, false);
}

void SuperFx::execute() {
    const uint8_t opcode = read_opcode();

    execute_opcode(opcode);

    if (r15_changed_) {
        r15_changed_ = false;
        return;
    }
    state_.r[15]++;
}

uint8_t SuperFx::read_opcode() {
    const uint8_t result = state_.program_read_buffer;

    state_.program_read_buffer = read_program_byte();

    return result;
}

uint8_t SuperFx::read_operand() {
    const uint8_t result = state_.program_read_buffer;

    state_.r[15]++;
    state_.program_read_buffer = read_program_byte();

    return result;
}

uint16_t SuperFx::read_src() const {
    return state_.r[state_.src_reg];
}

void SuperFx::write_dst(uint16_t value) {
    write_reg(state_.dst_reg, value);
}

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

void SuperFx::reset_prefix() {
    state_.flags.prefix = false;
    state_.flags.alt1 = false;
    state_.flags.alt2 = false;

    state_.src_reg = 0;
    state_.dst_reg = 0;
}

void SuperFx::invalidate_cache() {
    std::memset(cache_valid_, 0, sizeof(cache_valid_));
}
