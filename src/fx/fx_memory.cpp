/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"

// -------------------------------------------------------------
// SNES-side ownership
// -------------------------------------------------------------

bool SuperFx::rom_access_allowed() const {
    if (config_.chip == FxChip::FX3) return true;
    return !state_.flags.running || !state_.gsu_rom_access;
}

bool SuperFx::ram_access_allowed() const {
    if (config_.chip == FxChip::FX3) return true;
    return !state_.flags.running || !state_.gsu_ram_access;
}

// -------------------------------------------------------------
// SNES CPU ROM reads
// -------------------------------------------------------------
uint8_t SuperFx::blocked_rom_value(uint32_t addr) const {
    if (addr & 0x01) return 0x01;

    switch (addr & 0x0E) {
        case 0x04:
            return 0x04;

        case 0x0A:
            return 0x08;

        case 0x0E:
            return 0x0C;

        default:
            return 0x00;
    }
}

uint8_t SuperFx::cpu_rom_read(uint32_t addr) {
    if (!rom_access_allowed()) return blocked_rom_value(addr);

    return bus_.rom_read(bus_.context, addr);
}

// -------------------------------------------------------------
// SNES CPU RAM access
// -------------------------------------------------------------
uint8_t SuperFx::cpu_ram_read(uint32_t addr) {
    if (!ram_access_allowed()) return 0;

    return bus_.ram_read(bus_.context, addr);
}

// -------------------------------------------------------------
// GSU ROM/RAM ownership stall
// -------------------------------------------------------------

void SuperFx::wait_for_rom_access() {
    if (!state_.gsu_rom_access) {
        wait_for_rom_access_ = true;
        stopped_ = true;
    }
}

void SuperFx::wait_for_ram_access() {
    if (!state_.gsu_ram_access) {
        wait_for_ram_access_ = true;
        stopped_ = true;
    }
}

void SuperFx::update_running_state() {
    stopped_ = !state_.flags.running || wait_for_rom_access_ || wait_for_ram_access_;
}

// -------------------------------------------------------------
// Pending pipeline operations
// -------------------------------------------------------------

void SuperFx::wait_rom_operation() {
    if (state_.rom_delay) {
        step(state_.rom_delay);
    }
}

void SuperFx::wait_ram_operation() {
    if (state_.ram_delay) {
        step(state_.ram_delay);
    }
}

void SuperFx::cpu_ram_write(uint32_t addr, uint8_t value) {
    if (!ram_access_allowed()) return;

    bus_.ram_write(bus_.context, addr, value);
}

void SuperFx::step(uint32_t cycles) {
    state_.cycles += cycles;

    // ROM buffer pipeline
    if (state_.rom_delay) {
        const uint32_t amount = cycles < state_.rom_delay ? cycles : state_.rom_delay;
        state_.rom_delay -= (uint8_t)amount;

        if (state_.rom_delay == 0) {
            wait_for_rom_access();

            state_.rom_read_buffer = bus_.rom_read(bus_.context,
                                                   ((uint32_t)state_.rom_bank << 16) | state_.r[14]);
            state_.flags.rom_read_pending = false;
        }
    }

    // RAM write pipeline
    if (state_.ram_delay) {
        const uint32_t amount = cycles < state_.ram_delay ? cycles : state_.ram_delay;
        state_.ram_delay -= (uint8_t)amount;

        if (state_.ram_delay == 0) {
            wait_for_ram_access();

            bus_.ram_write(bus_.context,
                           ((uint32_t)state_.ram_bank << 16) | state_.ram_write_address,
                           state_.ram_write_value);
        }
    }
}

uint8_t SuperFx::read_rom_buffer() {
    wait_rom_operation();
    return state_.rom_read_buffer;
}

uint8_t SuperFx::read_ram(uint16_t address) {
    wait_ram_operation();
    wait_for_ram_access();

    return bus_.ram_read(bus_.context, ((uint32_t)state_.ram_bank << 16) | address);
}

void SuperFx::write_ram(uint16_t address, uint8_t value) {
    wait_ram_operation();

    state_.ram_delay = state_.clock_select ? 5 : 6;
    state_.ram_write_address = address;
    state_.ram_write_value = value;
}

uint8_t SuperFx::read_program_byte() {
    const uint16_t cache_addr = state_.r[15] - state_.cache_base;

    // Cache
    if (cache_addr < 512) {
        const uint8_t line = cache_addr >> 4;

        if (!cache_valid_[line]) {
            fill_cache_line(cache_addr & 0xFFF0);
        }

        step(state_.clock_select ? 1 : 2);

        return cache_[cache_addr];
    }

    // Uncached
    const uint8_t bank = state_.program_bank;
    if (bank <= config_.max_program_rom_bank) {
        wait_rom_operation();
        wait_for_rom_access();

        step(state_.clock_select ? 5 : 6);
        return bus_.rom_read(bus_.context, ((uint32_t)bank << 16) | state_.r[15]);
    } else {
        // Program execution outside the ROM bank range.
        // Only banks $70-$71 map to GSU RAM.
        // Other banks are unmapped.
        wait_ram_operation();
        wait_for_ram_access();

        step(state_.clock_select ? 5 : 6);

        if (bank == 0x70 || bank == 0x71)
            return bus_.ram_read(bus_.context,
                                 ((uint32_t)(bank - 0x70) << 16) | state_.r[15]);
        return 0x00; // Unmapped GSU address.
    }
}

void SuperFx::fill_cache_line(uint16_t cache_addr) {
    const uint16_t dest = cache_addr & 0x01F0;
    const uint8_t bank = state_.program_bank;

    if (bank <= config_.max_program_rom_bank) {
        wait_rom_operation();
        wait_for_rom_access();

        const uint32_t base = ((uint32_t)bank << 16) + state_.cache_base + dest;
        for (unsigned i = 0; i < 16; i++)
            cache_[dest + i] = bus_.rom_read(bus_.context, base + i);
    } else {
        wait_ram_operation();
        wait_for_ram_access();

        if (bank == 0x70 || bank == 0x71) {
            const uint32_t base = ((uint32_t)(bank - 0x70) << 16) + state_.cache_base + dest;
            for (unsigned i = 0; i < 16; i++)
                cache_[dest + i] = bus_.ram_read(bus_.context, base + i);
        } else {
            // Unmapped program bank.
            for (unsigned i = 0; i < 16; i++)
                cache_[dest + i] = 0x00;
        }
    }

    step((state_.clock_select ? 5 : 6) * 16);

    cache_valid_[cache_addr >> 4] = true;
}
