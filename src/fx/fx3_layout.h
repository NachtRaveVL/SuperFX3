/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#pragma once

#include <stdint.h>

// FX3 Technical Specifications v1.0 describes 128 KiB of FX SRAM in banks $70-$71
// and a 256 x 160 internal chunky framebuffer used by the real 8bpp PLOT/C2P path.
//
// This firmware deliberately does not allocate a second modeled chunky framebuffer.
// Like terminator2k2's working sd2snes FX3 implementation, the original GSU PLOT
// pixel cache is retained and SCMR mode 3 writes all eight planar bitplanes directly.
// The constants below therefore describe the CPU-visible planar area used by the clear
// commands, not a separately allocated hardware framebuffer.
namespace fx3_layout {

static constexpr uint32_t RAM_BANK_SIZE = 0x10000;
static constexpr uint32_t PLANAR_BASE = 0x10000;            // FX3 reset SCBR=$40 -> $10000 within shared FX SRAM.

static constexpr uint8_t X_TILES = 27;
static constexpr uint8_t Y_TILES = 18;
static constexpr uint16_t ACTIVE_WIDTH = static_cast<uint16_t>(X_TILES) * 8;
static constexpr uint16_t ACTIVE_HEIGHT = static_cast<uint16_t>(Y_TILES) * 8;

// NOTE: MesenCE's FX3 clear implementation writes bank $71 using 18 active tile rows
// in a 20-row column-major layout. FX3.PDF documents the 18-row command geometry but
// not the 20-row planar storage stride.
static constexpr uint8_t PLANAR_Y_TILE_STRIDE = 20;

static_assert(PLANAR_BASE + static_cast<uint32_t>(X_TILES) * PLANAR_Y_TILE_STRIDE * 64u <= RAM_BANK_SIZE * 2,
              "FX3 planar framebuffer must fit in FX SRAM bank $71.");
static_assert(ACTIVE_WIDTH == 216 && ACTIVE_HEIGHT == 144,
              "FX3 C2P active area must remain 27 x 18 tiles.");

} // namespace fx3_layout
