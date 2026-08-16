/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"
#include "pico.h"

// NOTE: Generic open-bus reads use Nintendo-style 0xFF instead of MesenCE's 0x00 fallback.

// NOTE: The FX and 65816 may access ROM simultaneously, so CPU ROM is never
// blocked by FX3 execution. GSU1/2 retain original ownership gating.
bool __not_in_flash_func(SuperFx::rom_access_allowed)() const {
    return config_.chip == FxChip::FX3 || !state_.flags.running || !state_.gsu_rom_access;
}

// NOTE: The FX and 65816 may access ROM simultaneously, so CPU ROM is never
// blocked by FX3 execution. GSU1/2 retain original ownership gating.
bool __not_in_flash_func(SuperFx::ram_access_allowed)() const {
    return config_.chip == FxChip::FX3 || !state_.flags.running || !state_.gsu_ram_access;
}

// Nintendo-documented and Mesen-compatible: blocked-ROM dummy data is derived from A0-A3.
uint8_t __not_in_flash_func(SuperFx::blocked_rom_value)(uint32_t addr) const {
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

// $00-$3F mirror 32 KiB ROM banks into both halves.
// $40-$5F map the first 2 MiB linearly; FX3 extends the linear window through $6F.
bool SuperFx::gsu_rom_offset(uint32_t address, uint32_t& offset) {
    const uint8_t bank = static_cast<uint8_t>(address >> 16);
    const uint16_t addr = static_cast<uint16_t>(address);

    if (bank <= 0x3F) {
        offset = (static_cast<uint32_t>(bank) << 15) | (addr & 0x7FFFu);
        return true;
    }

    if (bank >= 0x40 && bank <= config_.max_program_rom_bank) {
        offset = (static_cast<uint32_t>(bank - 0x40) << 16) | addr;
        return true;
    }

    return false;
}

uint8_t SuperFx::cpu_rom_read(uint32_t addr) {
    if (!rom_access_allowed())
        return blocked_rom_value(addr);
    if (!backend_.cpu_rom_read)
        return 0xFF;
    return backend_.cpu_rom_read(backend_.context, addr);
}

uint8_t __not_in_flash_func(SuperFx::cpu_ram_read)(uint32_t addr) {
    if (!ram_access_allowed()) {
        return 0xFF;
    }
    if (!backend_.ram_read)
        return 0xFF;
    return backend_.ram_read(backend_.context, addr);
}

// Mesen-derived for GSU1/2. FX3 ignores SCMR RON for FX-side ROM access.
// Randy's feedback and terminator2k2's working sd2snes FX3 implementation agree
// on this behavior, so FX3 must not enter the legacy ownership wait state here.
// NOTE: Legacy GSU1/2 has a separate issue: this callback backend can perform the physical
// access after WAIT is latched, which is not the same as MesenCE's logical memory model.
void SuperFx::wait_for_rom_access() {
    if (config_.chip == FxChip::FX3) return;

    if (!state_.gsu_rom_access) {
        wait_for_rom_access_ = true;
        stopped_ = true;
    }
}

// Mesen-derived for GSU1/2. FX3 likewise ignores SCMR RAN for FX-side RAM access.
void SuperFx::wait_for_ram_access() {
    if (config_.chip == FxChip::FX3) return;

    if (!state_.gsu_ram_access) {
        wait_for_ram_access_ = true;
        stopped_ = true;
    }
}

// Mesen-derived: closely follows MesenCE Gsu::UpdateRunningState().
void __not_in_flash_func(SuperFx::update_running_state)() {
    stopped_ = !state_.flags.running || wait_for_rom_access_ || wait_for_ram_access_;
}

void SuperFx::wait_rom_operation() {
    if (state_.rom_delay)
        step(state_.rom_delay);
}

void SuperFx::wait_ram_operation() {
    if (state_.ram_delay)
        step(state_.ram_delay);
}

void __not_in_flash_func(SuperFx::cpu_ram_write)(uint32_t addr, uint8_t value) {
    if (!ram_access_allowed() || !backend_.ram_write) return;
    backend_.ram_write(backend_.context, addr, value);
}

// Mesen-derived: closely follows MesenCE Gsu::Step(), adapted to callback-backed ROM/RAM.
void SuperFx::step(uint32_t cycles) {
    state_.cycles += cycles;

    // R14 reads and RAM writes are delayed pipelines, not immediate backend
    // operations. A large step can retire either one in the same call.

    // ROM buffer pipeline
    if (state_.rom_delay) {
        const uint32_t amount = cycles < state_.rom_delay ? cycles : state_.rom_delay;
        state_.rom_delay -= static_cast<uint8_t>(amount);

        if (state_.rom_delay == 0) {
            state_.rom_read_buffer = read_rom((static_cast<uint32_t>(state_.rom_bank) << 16) | state_.r[14]);
            state_.flags.rom_read_pending = false;
        }
    }

    // RAM write pipeline
    if (state_.ram_delay) {
        const uint32_t amount = cycles < state_.ram_delay ? cycles : state_.ram_delay;
        state_.ram_delay -= static_cast<uint8_t>(amount);

        if (state_.ram_delay == 0) {
            wait_for_ram_access();

            if (backend_.ram_write) {
                backend_.ram_write(backend_.context,
                                   (static_cast<uint32_t>(state_.ram_bank) << 16) | state_.ram_write_address,
                                   state_.ram_write_value);
            }
        }
    }
}

uint8_t SuperFx::read_rom(uint32_t address) {
    wait_rom_operation();

    uint32_t offset;
    if (!gsu_rom_offset(address, offset) || !backend_.rom_read)
        return 0xFF;

    wait_for_rom_access();

    return backend_.rom_read(backend_.context, offset);
}

// Mesen-derived: closely follows MesenCE Gsu::ReadRomBuffer().
uint8_t SuperFx::read_rom_buffer() {
    wait_rom_operation();

    return state_.rom_read_buffer;
}

// Mesen-derived: closely follows MesenCE Gsu::ReadRamBuffer().
uint8_t SuperFx::read_ram(uint16_t address) {
    wait_ram_operation();
    wait_for_ram_access();

    if (!backend_.ram_read)
        return 0xFF;

    return backend_.ram_read(backend_.context, (static_cast<uint32_t>(state_.ram_bank) << 16) | address);
}

uint8_t SuperFx::read_program_ram(uint32_t address) {
    wait_ram_operation();

    const uint8_t bank = static_cast<uint8_t>(address >> 16);
    if ((bank != 0x70 && bank != 0x71) || !backend_.ram_read)
        return 0xFF;

    wait_for_ram_access();

    const uint32_t offset = (static_cast<uint32_t>(bank - 0x70) << 16) | static_cast<uint16_t>(address);
    return backend_.ram_read(backend_.context, offset);
}

// Mesen-derived: closely follows MesenCE Gsu::WriteRam().
void SuperFx::write_ram(uint16_t address, uint8_t value) {
    wait_ram_operation();

    state_.ram_delay = state_.clock_select ? 5 : 6;
    state_.ram_write_address = address;
    state_.ram_write_value = value;
}

// Mesen-derived: closely follows MesenCE Gsu::ReadProgramByte(), using relative firmware RAM addresses.
uint8_t SuperFx::read_program_byte() {
    const uint16_t cache_addr = state_.r[15] - state_.cache_base;

    // Cache hits still consume the GSU's short access timing. Invalid lines are
    // filled as a 16-byte burst before the requested byte is returned.
    if (cache_addr < 512) {
        const uint8_t line = static_cast<uint8_t>(cache_addr >> 4);
        if (!cache_valid_[line])
            fill_cache_line(cache_addr & 0xFFF0);

        step(state_.clock_select ? 1 : 2);

        return cache_[cache_addr];
    }

    // Uncached program fetches use ROM through the configured maximum bank;
    // $70-$71 then become the GSU's executable RAM mapping.
    const uint8_t bank = state_.program_bank;
    if (bank <= config_.max_program_rom_bank) {
        wait_rom_operation();
        wait_for_rom_access();

        step(state_.clock_select ? 5 : 6);

        return read_rom((static_cast<uint32_t>(bank) << 16) | state_.r[15]);
    } else {
        wait_ram_operation();
        wait_for_ram_access();

        // Program execution outside the ROM bank range.
        // Only banks $70-$71 map to GSU RAM.
        // Other banks are unmapped.
        step(state_.clock_select ? 5 : 6);

        if (bank == 0x70 || bank == 0x71)
            return read_program_ram((static_cast<uint32_t>(bank) << 16) | state_.r[15]);
        return 0xFF; // Unmapped GSU address.
    }
}

// Mesen-derived: preserves MesenCE's 16-byte program-cache fill behavior.
void SuperFx::fill_cache_line(uint16_t cache_addr) {
    const uint16_t dest = cache_addr & 0x01F0;
    const uint8_t bank = state_.program_bank;

    if (bank <= config_.max_program_rom_bank) {
        wait_rom_operation();
        wait_for_rom_access();

        const uint32_t base = (static_cast<uint32_t>(bank) << 16) + state_.cache_base + dest;
        for (unsigned i = 0; i < 16; i++)
            cache_[dest + i] = read_rom(base + i);
    } else {
        wait_ram_operation();
        wait_for_ram_access();

        if (bank == 0x70 || bank == 0x71) {
            const uint32_t base = (static_cast<uint32_t>(bank) << 16) + state_.cache_base + dest;
            for (unsigned i = 0; i < 16; i++)
                cache_[dest + i] = read_program_ram(base + i);
        } else {
            // Unmapped program bank.
            for (unsigned i = 0; i < 16; i++)
                cache_[dest + i] = 0xFF;
        }
    }

    step((state_.clock_select ? 5 : 6) * 16);

    cache_valid_[cache_addr >> 4] = true;
}
