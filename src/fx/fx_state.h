/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#pragma once
#include <stdint.h>

enum class FxChip : uint8_t { GSU1, GSU2, FX3 };
enum class FxTiming : uint8_t { Accurate, Unlimited };

struct FxConfig {
    FxChip chip;
    FxTiming timing;
    uint8_t max_program_rom_bank;   // Mesen uses 0x5F normally, 0x6F for FX3.
};
extern const FxConfig fx1_config;
extern const FxConfig fx2_config;
extern const FxConfig fx3_config;

struct FxFlags {
    bool zero;
    bool carry;
    bool sign;
    bool overflow;
    bool running;
    bool rom_read_pending;
    bool alt1;
    bool alt2;
    bool imm_low;
    bool imm_high;
    bool prefix;
    bool irq;
};

struct FxPixelCache {
    uint8_t x;
    uint8_t y;
    uint8_t pixels[8];
    uint8_t valid_bits;
};

struct FxState {
    uint64_t cycles;
    uint16_t r[16];
    FxFlags flags;
    uint8_t register_latch;

    uint8_t program_bank;
    uint8_t rom_bank;
    uint8_t ram_bank;

    bool irq_disabled;
    bool high_speed;
    bool clock_select;
    bool backup_ram_enabled;

    uint8_t screen_base;
    uint8_t color_gradient;
    uint8_t plot_bpp;
    uint8_t screen_height;

    bool gsu_ram_access;
    bool gsu_rom_access;

    uint16_t cache_base;

    bool plot_transparent;
    bool plot_dither;
    bool color_high_nibble;
    bool color_freeze_high;
    bool object_mode;

    uint8_t color;

    uint8_t src_reg;
    uint8_t dst_reg;

    uint8_t rom_read_buffer;
    uint8_t rom_delay;

    uint8_t program_read_buffer;

    uint16_t ram_write_address;
    uint8_t ram_write_value;
    uint8_t ram_delay;

    uint16_t ram_address;

    FxPixelCache primary_cache;
    FxPixelCache secondary_cache;
};
