/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"
#include "fx3_layout.h"
#include "pico.h"

// FX3 clear-command layout
//
// MERGE commands 0-2 are intentionally handled as no-ops in fx3_commands.cpp because
// the retained GSU pixel cache already produces final 8bpp planar data. Commands 3-5
// still perform the FX3-specific patterned clears in CPU-visible bank $71.

static constexpr uint8_t FX3_CLEAR_PATTERN[64] = {
    0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
    0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
    0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF
};

// Mesen-derived: exact 64-byte clear pattern, destination bank $71, 18-row span,
// and 20-tile column stride follow Gsu::ClearCharFx3()/ProcessClearCommandFx3().
// NOTE: Bank-$71 destination, 20-row stride, and 64-byte clear pattern come from
// emulator implementations rather than FX3.PDF. Ordering is also undefined if CPU
// and FX write the same SRAM byte at the same time.
void SuperFx::fx3_clear(uint8_t first_block, uint8_t last_block) {
    // Valid planar tile columns are 0-26.
    if (first_block > 26 || last_block > 26 || first_block > last_block) return;

    const auto context = backend_.context;
    const auto ram_write = backend_.ram_write;
    if (!ram_write) return;

    const uint32_t block_stride = static_cast<uint32_t>(fx3_layout::PLANAR_Y_TILE_STRIDE) << 6;
    const uint32_t block_count = static_cast<uint32_t>(last_block - first_block) + 1;

    uint32_t row_address = fx3_layout::PLANAR_BASE +
        (static_cast<uint32_t>(first_block) * block_stride);

    // 18 active tile rows.
    for (uint32_t row = 0; row < 18; row++) {
        uint32_t address = row_address;

        for (uint32_t block = 0; block < block_count; block++) {
            for (uint32_t i = 0; i < 64; i++)
                ram_write(context, address + i, FX3_CLEAR_PATTERN[i]);

            address += block_stride;
        }

        row_address += 64;
    }
}
