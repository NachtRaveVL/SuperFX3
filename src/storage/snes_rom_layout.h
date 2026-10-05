/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

enum class SnesRomMap : uint8_t {
    LoRom,
    HiRom,
    ExLoRom,
    ExHiRom,
    Fx3Physical, // Prepacked bus image with the original full $70/$71 SRAM windows.
    Fx3, // Canonical FX3 ROM, up to 4 MiB; the first 3 MiB are FX-visible.
};

constexpr uint8_t SNES_CARTRIDGE_FX3 = 0x17u;
constexpr uint8_t SNES_CARTRIDGE_FX3_BATTERY = 0x18u;

struct SnesRomReader {
    void* context;
    uint8_t (*read)(void* context, uint32_t offset);
};

struct SnesRomInfo {
    SnesRomMap map;
    uint32_t size;
    uint32_t data_offset;
    uint32_t ram_size = 128u * 1024u;
    uint8_t cartridge_type = SNES_CARTRIDGE_FX3_BATTERY;
};

/// Detects LoROM/HiROM/ExLoROM/ExHiROM and canonical or striped FX3 ROMs.
bool snes_rom_detect(const SnesRomReader& reader, uint32_t file_size,
                     bool smc_extension, SnesRomInfo& info);
/// Maps a physical parallel-ROM address to a canonical source offset; SRAM overlays remain separate.
bool snes_rom_source_offset(const SnesRomInfo& info, uint32_t bus_address,
                            uint32_t& source_offset);

// Installed-map descriptor in an unused SNES WRAM bank of the parallel image.
// It consumes no QSPI space and no CPU-visible ROM bytes.
constexpr uint32_t SNES_ROM_DESCRIPTOR_ADDRESS = 0x7E0000u;
constexpr uint32_t SNES_ROM_DESCRIPTOR_SIZE = 16u;
void snes_rom_descriptor(const SnesRomInfo& info, uint8_t* bytes);
SnesRomInfo snes_rom_installed_info(const SnesRomReader& physical);
SnesRomMap snes_rom_installed_map(const SnesRomReader& physical);
bool snes_rom_has_persistent_ram(const SnesRomInfo& info);
