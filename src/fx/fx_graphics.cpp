/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"
#include "pico/platform/sections.h"

// -----------------------------------------------------------------------------
// FX3 framebuffer layout
// -----------------------------------------------------------------------------

// Chunky buffer:
//   216 x 144, 8bpp chunky
// Planar buffer:
//   SNES 8bpp tile format
//   27 x 18 active tiles
//   Stored using the GSU 160-pixel-high layout:
//       tile_index = x_tile * 20 + y_tile
//

static constexpr uint16_t FX3_WIDTH = 216;
static constexpr uint16_t FX3_HEIGHT = 144;
static constexpr uint8_t FX3_X_TILES = FX3_WIDTH / 8;
static constexpr uint8_t FX3_Y_TILES = FX3_HEIGHT / 8;

static constexpr uint32_t FX3_CHUNKY_BASE = 0x00000;
static constexpr uint32_t FX3_PLANAR_BASE = 0x10000;

static constexpr Fx3Framebuffer FX3_FB = {FX3_CHUNKY_BASE, FX3_WIDTH, FX3_PLANAR_BASE};

// GSU ScreenHeight=160 means each X tile column contains 20 tiles,
// even though FX3 only uses the first 18 here.
static constexpr uint8_t FX3_TILE_Y_STRIDE = 20;

// A/B/C each cover nine tile columns = 72 pixels.
static constexpr uint8_t FX3_REGION_TILES = 9;

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

uint8_t SuperFx::get_color(uint8_t value) {
    if (state_.color_high_nibble) return (state_.color & 0xF0) | (value >> 4);
    if (state_.color_freeze_high) return (state_.color & 0xF0) | (value & 0x0F);
    return value;
}

uint16_t SuperFx::get_tile_index(uint8_t x, uint8_t y) {
    const uint8_t mode = state_.object_mode ? 3 : state_.screen_height;

    switch (mode) {
        default:
        case 0:
            return ((x & 0xF8) << 1) + ((y & 0xF8) >> 3);
        case 1:
            return ((x & 0xF8) << 1) + ((x & 0xF8) >> 1) + ((y & 0xF8) >> 3);
        case 2:
            return ((x & 0xF8) << 1) + (x & 0xF8) + ((y & 0xF8) >> 3);
        case 3:
            return ((y & 0x80) << 2) + ((x & 0x80) << 1) + ((y & 0x78) << 1) + ((x & 0x78) >> 3);
    }
}

uint32_t SuperFx::get_tile_address(uint8_t x, uint8_t y) {
    const uint16_t tile = get_tile_index(x, y);
    return ((uint32_t)state_.screen_base << 10) +
           ((uint32_t)tile * ((uint32_t)state_.plot_bpp << 3)) +
           ((y & 0x07) * 2);
}

uint8_t SuperFx::read_pixel(uint8_t x, uint8_t y) {
    // Flush pending plot writes first.
    write_pixel_cache(state_.secondary_cache);
    write_pixel_cache(state_.primary_cache);

    const uint32_t tile_address = get_tile_address(x, y);
    const uint8_t bit = (x & 7) ^ 7;
    uint8_t result = 0;

    for (uint8_t plane = 0; plane < state_.plot_bpp; plane++) {
        const uint8_t byte_offset = ((plane >> 1) << 4) + (plane & 1);
        const uint8_t value = bus_.ram_read(bus_.context, tile_address + byte_offset);

        result |= ((value >> bit) & 1) << plane;
        step(state_.clock_select ? 5 : 6);
    }

    return result;
}

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

void SuperFx::draw_pixel(uint8_t x, uint8_t y) {
    if (!state_.plot_transparent && is_transparent_pixel()) {
        return;
    }

    uint8_t color = state_.color;
    if (state_.plot_dither && state_.plot_bpp != 8) {
        if ((x ^ y) & 1) color >>= 4;
        color &= 0x0F;
    }

    if (state_.primary_cache.x != (x & 0xF8) || state_.primary_cache.y != y) {
        flush_primary_cache(x, y);
    }

    const uint8_t bit = (x & 7) ^ 7;
    state_.primary_cache.pixels[bit] = color;
    state_.primary_cache.valid_bits |= 1u << bit;

    if (state_.primary_cache.valid_bits == 0xFF) {
        flush_primary_cache(x, y);
    }
}

void SuperFx::flush_primary_cache(uint8_t x, uint8_t y) {
    write_pixel_cache(state_.secondary_cache);
    state_.secondary_cache = state_.primary_cache;
    state_.primary_cache.valid_bits = 0;
    state_.primary_cache.x = x & 0xF8;
    state_.primary_cache.y = y;
}

void SuperFx::write_pixel_cache(FxPixelCache& cache) {
    if (cache.valid_bits == 0) return;

    const uint32_t tile_address = get_tile_address(cache.x, cache.y);
    for (uint8_t plane = 0; plane < state_.plot_bpp; plane++) {
        uint8_t value = 0;

        for (uint8_t bit = 0; bit < 8; bit++) {
            value |= ((cache.pixels[bit] >> plane) & 1) << bit;
        }

        const uint8_t byte_offset = ((plane >> 1) << 4) + (plane & 1);
        if (cache.valid_bits != 0xFF) {
            step(state_.clock_select ? 5 : 6);

            value &= cache.valid_bits;
            value |= bus_.ram_read(bus_.context, tile_address + byte_offset) & ~cache.valid_bits;
        }

        step(state_.clock_select ? 5 : 6);

        wait_for_ram_access();

        bus_.ram_write(bus_.context, tile_address + byte_offset, value);
    }

    cache.valid_bits = 0;
}

// -----------------------------------------------------------------------------
// Convert one row of 8 chunky pixels into eight SNES bitplane bytes.
// pixels[0] = leftmost pixel
// pixels[7] = rightmost pixel
// SNES tile format expects the leftmost pixel in bit 7.
// -----------------------------------------------------------------------------
static inline void fx3_c2p_row(const uint8_t pixels[8], uint8_t planes[8]) {
    planes[0] = (((pixels[0] >> 0) & 1) << 7) | (((pixels[1] >> 0) & 1) << 6) |
                (((pixels[2] >> 0) & 1) << 5) | (((pixels[3] >> 0) & 1) << 4) |
                (((pixels[4] >> 0) & 1) << 3) | (((pixels[5] >> 0) & 1) << 2) |
                (((pixels[6] >> 0) & 1) << 1) | (((pixels[7] >> 0) & 1) << 0);
    planes[1] = (((pixels[0] >> 1) & 1) << 7) | (((pixels[1] >> 1) & 1) << 6) |
                (((pixels[2] >> 1) & 1) << 5) | (((pixels[3] >> 1) & 1) << 4) |
                (((pixels[4] >> 1) & 1) << 3) | (((pixels[5] >> 1) & 1) << 2) |
                (((pixels[6] >> 1) & 1) << 1) | (((pixels[7] >> 1) & 1) << 0);
    planes[2] = (((pixels[0] >> 2) & 1) << 7) | (((pixels[1] >> 2) & 1) << 6) |
                (((pixels[2] >> 2) & 1) << 5) | (((pixels[3] >> 2) & 1) << 4) |
                (((pixels[4] >> 2) & 1) << 3) | (((pixels[5] >> 2) & 1) << 2) |
                (((pixels[6] >> 2) & 1) << 1) | (((pixels[7] >> 2) & 1) << 0);
    planes[3] = (((pixels[0] >> 3) & 1) << 7) | (((pixels[1] >> 3) & 1) << 6) |
                (((pixels[2] >> 3) & 1) << 5) | (((pixels[3] >> 3) & 1) << 4) |
                (((pixels[4] >> 3) & 1) << 3) | (((pixels[5] >> 3) & 1) << 2) |
                (((pixels[6] >> 3) & 1) << 1) | (((pixels[7] >> 3) & 1) << 0);
    planes[4] = (((pixels[0] >> 4) & 1) << 7) | (((pixels[1] >> 4) & 1) << 6) |
                (((pixels[2] >> 4) & 1) << 5) | (((pixels[3] >> 4) & 1) << 4) |
                (((pixels[4] >> 4) & 1) << 3) | (((pixels[5] >> 4) & 1) << 2) |
                (((pixels[6] >> 4) & 1) << 1) | (((pixels[7] >> 4) & 1) << 0);
    planes[5] = (((pixels[0] >> 5) & 1) << 7) | (((pixels[1] >> 5) & 1) << 6) |
                (((pixels[2] >> 5) & 1) << 5) | (((pixels[3] >> 5) & 1) << 4) |
                (((pixels[4] >> 5) & 1) << 3) | (((pixels[5] >> 5) & 1) << 2) |
                (((pixels[6] >> 5) & 1) << 1) | (((pixels[7] >> 5) & 1) << 0);
    planes[6] = (((pixels[0] >> 6) & 1) << 7) | (((pixels[1] >> 6) & 1) << 6) |
                (((pixels[2] >> 6) & 1) << 5) | (((pixels[3] >> 6) & 1) << 4) |
                (((pixels[4] >> 6) & 1) << 3) | (((pixels[5] >> 6) & 1) << 2) |
                (((pixels[6] >> 6) & 1) << 1) | (((pixels[7] >> 6) & 1) << 0);
    planes[7] = (((pixels[0] >> 7) & 1) << 7) | (((pixels[1] >> 7) & 1) << 6) |
                (((pixels[2] >> 7) & 1) << 5) | (((pixels[3] >> 7) & 1) << 4) |
                (((pixels[4] >> 7) & 1) << 3) | (((pixels[5] >> 7) & 1) << 2) |
                (((pixels[6] >> 7) & 1) << 1) | (((pixels[7] >> 7) & 1) << 0);
}

// -----------------------------------------------------------------------------
// FX3 chunky -> planar conversion
// region:
//     0 = A = X 0..71
//     1 = B = X 72..143
//     2 = C = X 144..215
// -----------------------------------------------------------------------------
void __not_in_flash_func(SuperFx::fx3_chunky_to_planar)(uint8_t region) {
    if (region >= 3) return;

    const uint8_t first_x_tile = region * FX3_REGION_TILES;
    const uint8_t last_x_tile = first_x_tile + FX3_REGION_TILES;

    if (last_x_tile > FX3_X_TILES) return;

    uint8_t pixels[8];
    uint8_t planes[8];

    for (uint8_t x_tile = first_x_tile; x_tile < last_x_tile; x_tile++) {
        for (uint8_t y_tile = 0; y_tile < FX3_Y_TILES; y_tile++) {
            // GSU 160-high tile arrangement:
            //     tile = x * 20 + y
            // Each 8bpp SNES tile occupies 64 bytes.
            const uint32_t tile_index = (uint32_t)x_tile * FX3_TILE_Y_STRIDE + y_tile;
            const uint32_t dst_tile = FX3_FB.planar_base + tile_index * 64;

            for (uint8_t row = 0; row < 8; row++) {
                // Chunky source:
                //     byte = framebuffer[y][x]
                // One byte = one complete 8-bit pixel.
                const uint16_t pixel_y = (uint16_t)y_tile * 8 + row;
                const uint16_t pixel_x = (uint16_t)x_tile * 8;
                const uint32_t src = FX3_FB.chunky_base +
                    (uint32_t)pixel_y * FX3_FB.chunky_pitch + pixel_x;

                pixels[0] = bus_.ram_read(bus_.context, src + 0);
                pixels[1] = bus_.ram_read(bus_.context, src + 1);
                pixels[2] = bus_.ram_read(bus_.context, src + 2);
                pixels[3] = bus_.ram_read(bus_.context, src + 3);
                pixels[4] = bus_.ram_read(bus_.context, src + 4);
                pixels[5] = bus_.ram_read(bus_.context, src + 5);
                pixels[6] = bus_.ram_read(bus_.context, src + 6);
                pixels[7] = bus_.ram_read(bus_.context, src + 7);
                fx3_c2p_row(pixels, planes);

                // SNES 8bpp tile arrangement:
                // $00-$0F = planes 0/1
                // $10-$1F = planes 2/3
                // $20-$2F = planes 4/5
                // $30-$3F = planes 6/7
                // Two bytes per scanline within each plane pair.
                const uint32_t line = (uint32_t)row << 1;
                bus_.ram_write(bus_.context, dst_tile + 0x00 + line, planes[0]);
                bus_.ram_write(bus_.context, dst_tile + 0x00 + line + 1, planes[1]);
                bus_.ram_write(bus_.context, dst_tile + 0x10 + line, planes[2]);
                bus_.ram_write(bus_.context, dst_tile + 0x10 + line + 1, planes[3]);
                bus_.ram_write(bus_.context, dst_tile + 0x20 + line, planes[4]);
                bus_.ram_write(bus_.context, dst_tile + 0x20 + line + 1, planes[5]);
                bus_.ram_write(bus_.context, dst_tile + 0x30 + line, planes[6]);
                bus_.ram_write(bus_.context, dst_tile + 0x30 + line + 1, planes[7]);
            }
        }
    }
}

void SuperFx::fx3_clear(uint8_t first_block, uint8_t last_block) {
    // Valid framebuffer block columns are 0-26.
    if (first_block > 26 || last_block > 26 || first_block > last_block) {
        return;
    }

    // 18 active tile rows.
    for (uint8_t row = 0; row < 18; row++) {
        const uint32_t row_offset = static_cast<uint32_t>(row) * 64;

        for (uint8_t block = first_block; block <= last_block; block++) {
            const uint32_t address = FX3_FB.planar_base +
                row_offset + static_cast<uint32_t>(block) * 20 * 64;

            for (uint32_t i = 0; i < 64; i++) {
                bus_.ram_write(bus_.context, address + i, FX3_CLEAR_PATTERN[i]);
            }
        }
    }
}
