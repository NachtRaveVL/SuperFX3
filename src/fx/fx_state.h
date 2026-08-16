/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#pragma once
#include <stdint.h>

enum class FxChip : uint8_t {
    GSU1,                              ///< Original Super FX / GSU-1 hardware.
    GSU2,                              ///< Second-generation Super FX / GSU-2 hardware.
    FX3                                ///< SuperFX3 hardware with the extended feature set.
};

enum class FxTiming : uint8_t {
    Accurate,                          ///< Run using emulated GSU timing and cycle accounting.
    Unlimited                          ///< Run without real-time cycle limiting.
};

struct FxConfig {
    FxChip chip;                       ///< SuperFX hardware revision being emulated.
    FxTiming timing;                   ///< Timing model used by the execution core.
    uint8_t max_program_rom_bank;      ///< Highest ROM bank available for program execution ($6F is PDF-defined for FX3).
};

extern const FxConfig fx1_config;      ///< SuperFX1 configuration
extern const FxConfig fx2_config;      ///< SuperFX2 configuration
extern const FxConfig fx3_config;      ///< SuperFX3 configuration

struct FxFlags {
    bool zero;                         ///< Zero result flag.
    bool carry;                        ///< Carry or borrow flag.
    bool sign;                         ///< Sign flag reflecting bit 15 of the result.
    bool overflow;                     ///< Signed arithmetic overflow flag.

    bool running;                      ///< Set while the GSU is actively executing.
    bool rom_read_pending;             ///< Buffered ROM read through R14 is still pending.

    bool alt1;                         ///< ALT1 instruction prefix state.
    bool alt2;                         ///< ALT2 instruction prefix state.

    bool imm_low;                      ///< Immediate low-byte instruction mode.
    bool imm_high;                     ///< Immediate high-byte instruction mode.

    bool prefix;                       ///< WITH prefix is active.
    bool irq;                          ///< GSU interrupt request is pending.
};

struct FxPixelCache {
    uint8_t x;                         ///< X coordinate of the cached eight-pixel span.
    uint8_t y;                         ///< Y coordinate of the cached scanline.
    uint8_t pixels[8];                 ///< Pixel colors waiting to be written as planar data.
    uint8_t valid_bits;                ///< One bit per pixel indicating valid cached data.
};

struct FxState {
    uint64_t cycles;                   ///< Total number of emulated GSU cycles elapsed.
    uint16_t r[16];                    ///< General-purpose registers R0-R15.
    FxFlags flags;                     ///< Processor status and instruction-prefix flags.
    uint8_t register_latch;            ///< Holds the low byte until a CPU register write is committed.

    uint8_t program_bank;              ///< Program bank used for instruction fetches.
    uint8_t rom_bank;                  ///< ROM bank used by buffered R14 reads.
    uint8_t ram_bank;                  ///< RAM bank used by GSU memory operations.

    bool irq_disabled;                 ///< Prevents STOP from asserting the external GSU IRQ.
    bool high_speed;                   ///< Selects the GSU high-speed operating mode.
    bool clock_select;                 ///< Selects the GSU memory-access timing rate.
    bool backup_ram_enabled;           ///< Enables access to cartridge backup RAM.

    uint8_t screen_base;               ///< Base page of the GSU planar framebuffer.
    uint8_t color_gradient;            ///< SCMR color-depth selection.
    uint8_t plot_bpp;                  ///< Active plot depth of 2, 4, or 8 bits per pixel.
    uint8_t screen_height;             ///< Screen-height mode used for tile addressing.

    bool gsu_ram_access;               ///< SNES has granted RAM access to the GSU.
    bool gsu_rom_access;               ///< SNES has granted ROM access to the GSU.

    uint16_t cache_base;               ///< Base address of the 512-byte instruction cache.

    bool plot_transparent;             ///< Controls whether color zero is written by PLOT.
    bool plot_dither;                  ///< Enables alternating-color dithering during PLOT.
    bool color_high_nibble;            ///< Uses the current color register high nibble.
    bool color_freeze_high;            ///< Preserves the current color register high nibble.
    bool object_mode;                  ///< Selects OBJ-style tile addressing.

    uint8_t color;                     ///< Current color used by graphics instructions.

    uint8_t src_reg;                   ///< Register currently selected as the instruction source.
    uint8_t dst_reg;                   ///< Register currently selected as the instruction destination.

    uint8_t rom_read_buffer;           ///< Most recently completed buffered ROM read value.
    uint8_t rom_delay;                 ///< Cycles remaining before the pending ROM read completes.

    uint8_t program_read_buffer;       ///< Prefetched byte forming the GSU instruction pipeline.

    uint16_t ram_write_address;        ///< Address of the pending delayed RAM write.
    uint8_t ram_write_value;           ///< Data associated with the pending delayed RAM write.
    uint8_t ram_delay;                 ///< Cycles remaining before the pending RAM write completes.

    uint16_t ram_address;              ///< Current RAM address used by load/store instructions.

    FxPixelCache primary_cache;        ///< Cache receiving the current group of plotted pixels.
    FxPixelCache secondary_cache;      ///< Previous pixel group waiting to be written to RAM.
};
