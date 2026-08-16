/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"

// Mesen-derived: closely follows MesenCE Gsu::GetColor().
uint8_t SuperFx::get_color(uint8_t value) {
    if (state_.color_high_nibble) return (state_.color & 0xF0) | (value >> 4);
    if (state_.color_freeze_high) return (state_.color & 0xF0) | (value & 0x0F);
    return value;
}

// Mesen-derived: closely follows MesenCE Gsu::GetTileIndex().
uint16_t SuperFx::get_tile_index(uint8_t x, uint8_t y) {
    const uint8_t mode = state_.object_mode ? 3 : state_.screen_height;

    switch (mode) {
        default:
        case 0:
            return static_cast<uint16_t>(((x & 0xF8) << 1) + ((y & 0xF8) >> 3));
        case 1:
            return static_cast<uint16_t>(((x & 0xF8) << 1) + ((x & 0xF8) >> 1) + ((y & 0xF8) >> 3));
        case 2:
            return static_cast<uint16_t>(((x & 0xF8) << 1) + (x & 0xF8) + ((y & 0xF8) >> 3));
        case 3:
            return static_cast<uint16_t>(((y & 0x80) << 2) + ((x & 0x80) << 1) +
                                         ((y & 0x78) << 1) + ((x & 0x78) >> 3));
    }
}

// Mesen-derived: closely follows MesenCE Gsu::GetTileAddress(), with RAM-relative addressing.
uint32_t SuperFx::get_tile_address(uint8_t x, uint8_t y) {
    const uint16_t tile = get_tile_index(x, y);
    return (static_cast<uint32_t>(state_.screen_base) << 10) +
           (static_cast<uint32_t>(tile) * (static_cast<uint32_t>(state_.plot_bpp) << 3)) +
           ((y & 0x07) * 2);
}

// Mesen-derived: closely follows MesenCE Gsu::ReadPixel().
// FX3 keeps this planar read path in the compatibility implementation below.
uint8_t SuperFx::read_pixel(uint8_t x, uint8_t y) {
    // Flush pending plot writes first.
    write_pixel_cache(state_.secondary_cache);
    write_pixel_cache(state_.primary_cache);

    const uint32_t tile_address = get_tile_address(x, y);
    const uint8_t bit = (x & 7) ^ 7;
    uint8_t result = 0;

    for (uint8_t plane = 0; plane < state_.plot_bpp; plane++) {
        const uint8_t byte_offset = static_cast<uint8_t>(((plane >> 1) << 4) + (plane & 1));
        const uint8_t value = backend_.ram_read(backend_.context, tile_address + byte_offset);

        result |= static_cast<uint8_t>(((value >> bit) & 1u) << plane);
        step(state_.clock_select ? 5 : 6);
    }

    return result;
}

// Mesen-derived: closely follows MesenCE Gsu::IsTransparentPixel().
bool SuperFx::is_transparent_pixel() const {
    const uint8_t color = state_.color_freeze_high ? (state_.color & 0x0F) : state_.color;

    switch (state_.plot_bpp) {
        case 2:
            return (color & 0x03) == 0;
        case 4:
            return (color & 0x0F) == 0;
        case 8:
            return color == 0;
        default:
            return true;
    }
}

// Mesen-derived: closely follows the original GSU PLOT/pixel-cache path.
// FX3 hardware uses an internal chunky framebuffer, but the working sd2snes FX3
// implementation by terminator2k2 retains this cache and lets SCMR mode 3 flush
// all eight bitplanes directly. MERGE C2P commands can therefore be no-ops here.
void SuperFx::draw_pixel(uint8_t x, uint8_t y) {
    if (!state_.plot_transparent && is_transparent_pixel()) return;

    uint8_t color = state_.color;
    if (state_.plot_dither && state_.plot_bpp != 8) {
        if ((x ^ y) & 1) color >>= 4;
        color &= 0x0F;
    }

    if (state_.primary_cache.x != (x & 0xF8) || state_.primary_cache.y != y)
        flush_primary_cache(x, y);

    const uint8_t bit = (x & 7) ^ 7;
    state_.primary_cache.pixels[bit] = color;
    state_.primary_cache.valid_bits |= static_cast<uint8_t>(1u << bit);

    if (state_.primary_cache.valid_bits == 0xFF)
        flush_primary_cache(x, y);
}

// Mesen-derived: closely follows MesenCE's primary/secondary plot-cache handoff.
void SuperFx::flush_primary_cache(uint8_t x, uint8_t y) {
    write_pixel_cache(state_.secondary_cache);

    state_.secondary_cache = state_.primary_cache;

    state_.primary_cache.valid_bits = 0;
    state_.primary_cache.x = x & 0xF8;
    state_.primary_cache.y = y;
}

// Mesen-derived: closely follows MesenCE Gsu::WritePixelCache().
// plot_bpp == 8 naturally extends the same bit transpose through planes 0-7.
void SuperFx::write_pixel_cache(FxPixelCache& cache) {
    if (cache.valid_bits == 0) return;

    const uint32_t tile_address = get_tile_address(cache.x, cache.y);
    for (uint8_t plane = 0; plane < state_.plot_bpp; plane++) {
        uint8_t value = 0;

        for (uint8_t bit = 0; bit < 8; bit++)
            value |= static_cast<uint8_t>(((cache.pixels[bit] >> plane) & 1u) << bit);

        const uint8_t byte_offset = static_cast<uint8_t>(((plane >> 1) << 4) + (plane & 1));
        if (cache.valid_bits != 0xFF) {
            step(state_.clock_select ? 5 : 6);

            value &= cache.valid_bits;
            const uint8_t keep_mask = static_cast<uint8_t>(~cache.valid_bits);
            value |= static_cast<uint8_t>(
                backend_.ram_read(backend_.context, tile_address + byte_offset) & keep_mask
            );
        }

        step(state_.clock_select ? 5 : 6);

        wait_for_ram_access();

        backend_.ram_write(backend_.context, tile_address + byte_offset, value);
    }

    cache.valid_bits = 0;
}
