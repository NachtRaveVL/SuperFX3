/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "snes_rom_layout.h"

namespace {
constexpr uint32_t KIB = 1024u;
constexpr uint32_t MIB = 1024u * KIB;

uint16_t read16(const SnesRomReader& reader, uint32_t offset) {
    return static_cast<uint16_t>(reader.read(reader.context, offset)) |
           (static_cast<uint16_t>(reader.read(reader.context, offset + 1u)) << 8);
}

int header_score(const SnesRomReader& reader, uint32_t offset, SnesRomMap map) {
    const uint8_t map_mode = reader.read(reader.context, offset + 0x15u);
    const uint16_t complement = read16(reader, offset + 0x1Cu);
    const uint16_t checksum = read16(reader, offset + 0x1Eu);
    const uint16_t reset_vector = read16(reader, offset + 0x3Cu);
    int score = 0;

    if (checksum != 0 && checksum != 0xFFFFu &&
        static_cast<uint16_t>(checksum ^ complement) == 0xFFFFu)
        score += 8;
    const bool hirom = map == SnesRomMap::HiRom || map == SnesRomMap::ExHiRom;
    if (((map_mode & 1u) != 0) == hirom)
        score += 4;
    if (map == SnesRomMap::ExHiRom && (map_mode & 0x0Fu) == 0x05u)
        score += 3;
    if (map == SnesRomMap::ExLoRom &&
        ((map_mode & 0x0Fu) == 0x00u || (map_mode & 0x0Fu) == 0x02u))
        score += 2;
    if ((map_mode & 0x20u) != 0)
        score += 1;
    if (reset_vector >= 0x8000u)
        score += 2;

    unsigned printable = 0;
    for (uint32_t index = 0; index < 21u; ++index) {
        const uint8_t value = reader.read(reader.context, offset + index);
        if (value == 0 || value == ' ' || (value >= 0x21u && value <= 0x7Eu))
            ++printable;
    }
    if (printable >= 19u)
        score += 2;
    return score;
}

uint32_t mirror_offset(uint32_t size, uint32_t offset) {
    if (offset < size)
        return offset;

    uint32_t mask = 1u << 31;
    while ((offset & mask) == 0)
        mask >>= 1;
    if (size <= (offset & mask))
        return mirror_offset(size, offset - mask);
    return mask + mirror_offset(size - mask, offset - mask);
}
}

bool snes_rom_detect(const SnesRomReader& reader, uint32_t file_size,
                     bool smc_extension, SnesRomInfo& info) {
    (void)smc_extension;
    if (!reader.read || file_size == 0)
        return false;

    const bool copier_header = (file_size % (32u * KIB)) == 512u;
    const uint32_t data_offset = copier_header ? 512u : 0u;
    const uint32_t data_size = file_size - data_offset;
    if (data_size == 0 || data_size > 8u * MIB || (data_size % (32u * KIB)) != 0)
        return false;

    if (data_size > 4u * MIB) {
        const int exlo_low = header_score(reader, data_offset + 0x7FC0u,
                                          SnesRomMap::ExLoRom);
        const int exlo_high = header_score(reader, data_offset + 0x407FC0u,
                                           SnesRomMap::ExLoRom);
        const int exhi = data_size >= 0x410000u ?
            header_score(reader, data_offset + 0x40FFC0u, SnesRomMap::ExHiRom) : -1;
        const int exlo = exlo_low > exlo_high ? exlo_low : exlo_high;
        if (exlo < 8 && exhi < 8)
            return false;
        info = {exhi > exlo ? SnesRomMap::ExHiRom : SnesRomMap::ExLoRom,
                data_size, data_offset};
    } else {
        const int lo_score = header_score(reader, data_offset + 0x7FC0u,
                                          SnesRomMap::LoRom);
        const int hi_score = data_size >= 0x10000u ?
            header_score(reader, data_offset + 0xFFC0u, SnesRomMap::HiRom) : -1;
        if (lo_score < 8 && hi_score < 8)
            return false;
        info = {hi_score > lo_score ? SnesRomMap::HiRom : SnesRomMap::LoRom,
                data_size, data_offset};
    }
    return true;
}

bool snes_rom_source_offset(const SnesRomInfo& info, uint32_t bus_address,
                            uint32_t& source_offset) {
    if (bus_address >= 16u * MIB || info.size == 0)
        return false;
    const uint8_t bank = static_cast<uint8_t>(bus_address >> 16);
    const uint16_t address = static_cast<uint16_t>(bus_address);
    if (bank == 0x7Eu || bank == 0x7Fu)
        return false;
    if ((bank <= 0x3Fu || (bank >= 0x80u && bank <= 0xBFu)) && address < 0x8000u)
        return false;

    uint32_t linear = 0;
    if (info.map == SnesRomMap::LoRom) {
        linear = (static_cast<uint32_t>(bank & 0x7Fu) << 15) |
                 (address & 0x7FFFu);
    } else if (info.map == SnesRomMap::HiRom) {
        linear = (static_cast<uint32_t>(bank & 0x3Fu) << 16) | address;
    } else if (info.map == SnesRomMap::ExLoRom) {
        linear = (static_cast<uint32_t>(bank ^ 0x80u) << 15) |
                 (address & 0x7FFFu);
    } else {
        linear = (static_cast<uint32_t>(bank & 0x3Fu) << 16) | address |
                 (bank < 0x80u ? 0x400000u : 0u);
    }
    source_offset = mirror_offset(info.size, linear);
    return true;
}

void snes_rom_descriptor(const SnesRomInfo& info, uint8_t* bytes) {
    const uint32_t words[] = {0x504D3353u, static_cast<uint32_t>(info.map),
                              info.size, ~(0x504D3353u ^ static_cast<uint32_t>(info.map) ^ info.size)};
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t byte = 0; byte < 4; ++byte)
            bytes[i * 4u + byte] = static_cast<uint8_t>(words[i] >> (byte * 8u));
    }
}

SnesRomMap snes_rom_installed_map(const SnesRomReader& physical) {
    if (!physical.read)
        return SnesRomMap::Fx3Physical;
    uint32_t words[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t byte = 0; byte < 4; ++byte)
            words[i] |= static_cast<uint32_t>(physical.read(physical.context,
                SNES_ROM_DESCRIPTOR_ADDRESS + i * 4u + byte)) << (byte * 8u);
    }
    if (words[0] != 0x504D3353u || words[1] > static_cast<uint32_t>(SnesRomMap::ExHiRom) ||
        !words[2] || words[2] > 8u * MIB || (words[2] % (32u * KIB)) != 0 ||
        words[3] != ~(words[0] ^ words[1] ^ words[2]))
        return SnesRomMap::Fx3Physical;
    return static_cast<SnesRomMap>(words[1]);
}
