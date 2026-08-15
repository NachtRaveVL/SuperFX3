/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"

uint8_t SuperFx::flags_low() const {
    return (state_.flags.zero << 1) | (state_.flags.carry << 2) |
           (state_.flags.sign << 3) | (state_.flags.overflow << 4) |
           (state_.flags.running << 5) | (state_.flags.rom_read_pending << 6);
}

uint8_t SuperFx::flags_high() const {
    return (state_.flags.alt1 << 0) | (state_.flags.alt2 << 1) |
           (state_.flags.imm_low << 2) | (state_.flags.imm_high << 3) |
           (state_.flags.prefix << 4) | (state_.flags.irq << 7);
}

// -------------------------------------------------------------
// SNES CPU -> GSU register read
// Accept either:
//   $3000-$33FF normal GSU
// or
//   $7000-$73FF FX3 external mapping
// Masking converts either into canonical $3000-$33FF.
// -------------------------------------------------------------
uint8_t SuperFx::cpu_read(uint16_t addr) {
    addr &= 0x33FF;

    // While executing, only SFR/VCR are readable.
    // FX3 additionally allows R15 polling.
    if (state_.flags.running) {
        bool allowed = addr == 0x3030 || addr == 0x3031 || addr == 0x303B;

        // FX3 has no completion IRQ.
        // R15 may be polled while running.
        if (config_.chip == FxChip::FX3 && (addr == 0x301E || addr == 0x301F))
            allowed = true;

        if (!allowed) return 0;
    }

    // R0-R15
    if (addr >= 0x3000 && addr <= 0x301F) {
        const uint8_t reg = (addr >> 1) & 0x0F;

        if (addr & 1) return state_.r[reg] >> 8;
        return (uint8_t)state_.r[reg];
    }

    switch (addr) {
        // SFR low
        case 0x3030:
            return flags_low();

        // SFR high
        // Reading clears GSU IRQ.
        case 0x3031: {
            const uint8_t result = flags_high();

            state_.flags.irq = false;

            if (bus_.set_irq) bus_.set_irq(bus_.context, false);

            return result;
        }

        case 0x3034: // PBR
            return state_.program_bank;

        case 0x3036: // ROMBR
            return state_.rom_bank;

        case 0x303B: // VCR
            return config_.chip == FxChip::FX3 ? 0x52 : 0x04;

        case 0x303C: // RAMBR
            return state_.ram_bank;

        case 0x303E: // CBR
            return (uint8_t)state_.cache_base;

        case 0x303F:
            return state_.cache_base >> 8;
    }

    // Program cache: $3100-$32FF
    if (addr >= 0x3100 && addr <= 0x32FF) {
        const uint16_t cache_addr = (state_.cache_base + (addr - 0x3100)) & 0x01FF;
        return cache_[cache_addr];
    }

    // Open bus eventually.
    return 0;
}

void SuperFx::cpu_write(uint16_t addr, uint8_t value) {
    addr &= 0x33FF;

    // While executing, SNES may only modify SFR and SCMR.
    if (state_.flags.running && addr != 0x3030 && addr != 0x303A)
        return;

    // R0-R15
    if (addr >= 0x3000 && addr <= 0x301F) {
        const uint8_t reg = (addr >> 1) & 0x0F;

        if (!(addr & 1)) {
            // Low byte latch
            state_.register_latch = value;

        } else {
            // High byte commits entire register.
            state_.r[reg] = state_.register_latch | ((uint16_t)value << 8);

            // R14 initiates ROM-buffer fetch.
            if (reg == 14) {
                state_.flags.rom_read_pending = true;
                state_.rom_delay = state_.clock_select ? 5 : 6;
            }

            // Writing R15 high starts execution.
            if (reg == 15) {
                state_.flags.running = true;
                update_running_state();
            }
        }

        return;
    }

    switch (addr) {
        // SFR
        case 0x3030: {
            const bool was_running = state_.flags.running;
            state_.flags.zero = (value & 0x02) != 0;
            state_.flags.carry = (value & 0x04) != 0;
            state_.flags.sign = (value & 0x08) != 0;
            state_.flags.overflow = (value & 0x10) != 0;
            state_.flags.running = (value & 0x20) != 0;

            // Stopping the GSU clears program cache state.
            if (was_running && !state_.flags.running) {
                state_.cache_base = 0;
                invalidate_cache();
            }

            update_running_state();
            break;
        }

        // BRAMR
        case 0x3033:
            state_.backup_ram_enabled = (value & 0x01) != 0;
            break;

        // PBR
        case 0x3034:
            state_.program_bank = value & 0x7F;
            invalidate_cache();
            break;

        // CFGR
        case 0x3037:
            state_.high_speed = (value & 0x20) != 0;
            state_.irq_disabled = (value & 0x80) != 0;
            break;

        // SCBR
        case 0x3038:
            state_.screen_base = value;
            break;

        // CLSR
        case 0x3039:
            state_.clock_select = (value & 0x01) != 0;
            break;

        // SCMR
        case 0x303A:
            state_.color_gradient = value & 0x03;
            switch (state_.color_gradient) {
                case 0:
                    state_.plot_bpp = 2;
                    break;

                case 1:
                case 2:
                    state_.plot_bpp = 4;
                    break;

                case 3:
                    state_.plot_bpp = 8;
                    break;
            }

            state_.screen_height = ((value & 0x04) >> 2) | ((value & 0x20) >> 4);
            state_.gsu_ram_access = (value & 0x08) != 0;
            state_.gsu_rom_access = (value & 0x10) != 0;

            // SNES just gave GSU ownership.
            if (state_.gsu_ram_access) wait_for_ram_access_ = false;
            if (state_.gsu_rom_access) wait_for_rom_access_ = false;

            update_running_state();
            break;
    }

    // Program cache writes
    if (addr >= 0x3100 && addr <= 0x32FF) {
        const uint16_t cache_addr = (state_.cache_base + (addr - 0x3100)) & 0x01FF;

        cache_[cache_addr] = value;

        // Cache line becomes valid once its final byte has been written.
        if ((cache_addr & 0x0F) == 0x0F) {
            cache_valid_[cache_addr >> 4] = true;
        }
    }
}
