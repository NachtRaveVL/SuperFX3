/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "snes_rom_layout.h"

namespace {
constexpr uint32_t KIB = 1024u;
constexpr uint32_t MIB = 1024u * KIB;
constexpr uint32_t DESCRIPTOR_EXTENDED = 0x80000000u;

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

uint32_t header_ram_size(const SnesRomReader& reader, uint32_t offset,
                         uint8_t cartridge_type) {
    uint8_t code = reader.read(reader.context, offset + 0x18u);
    if (cartridge_type >= 0x13u && cartridge_type <= 0x18u) {
        const uint8_t expansion = reader.read(reader.context, offset - 3u);
        if (expansion <= 7u)
            code = expansion;
    }
    if (code == 0)
        return 0;
    return code <= 7u ? 1u << (code + 10u) : 128u * KIB;
}

uint8_t ram_size_code(uint32_t size) {
    if (!size)
        return 0;
    uint8_t code = 1;
    for (uint32_t bytes = 2u * KIB; bytes < size && code < 7u; bytes <<= 1)
        ++code;
    return code;
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

    const auto detect_at = [&reader, file_size](uint32_t data_offset,
                                                SnesRomInfo& candidate,
                                                int& candidate_score) {
        if (data_offset >= file_size)
            return false;
        uint32_t data_size = file_size - data_offset;
        bool trailer = false;
        if (data_size == 8u * MIB + 256u) {
            trailer = true;
            for (uint32_t i = 8u * MIB; i < data_size; ++i) {
                if (reader.read(reader.context, data_offset + i) != 0xFF)
                    return false;
            }
            data_size -= 256u;
        }
        if (data_size == 0 || data_size > 8u * MIB ||
            (data_size % (32u * KIB)) != 0)
            return false;

        const uint32_t canonical = data_offset +
            (data_size == 8u * MIB ? 4u * MIB : 0u);
        const auto fx3_score = [&reader](uint32_t base) {
            const uint8_t type = reader.read(reader.context, base + 0x7FD6u);
            return (type == SNES_CARTRIDGE_FX3 || type == SNES_CARTRIDGE_FX3_BATTERY) ?
                header_score(reader, base + 0x7FC0u, SnesRomMap::LoRom) : -1;
        };
        const int fx_score = fx3_score(canonical);
        if (fx_score >= 8) {
            const uint32_t size = data_size == 8u * MIB ? 4u * MIB : data_size;
            uint8_t size_field = 0;
            for (uint32_t capacity = KIB; capacity < size; capacity <<= 1)
                ++size_field;
            if (size > 4u * MIB ||
                reader.read(reader.context, canonical + 0x7FD7u) != size_field)
                return false;
            if (data_size == 8u * MIB) {
                for (uint32_t i = 0; i < 2u * MIB; ++i) {
                    const uint32_t physical = ((i & ~0x7FFFu) << 1) | (i & 0x7FFFu);
                    const uint8_t value = reader.read(reader.context, canonical + i);
                    if (reader.read(reader.context, data_offset + physical) != value ||
                        reader.read(reader.context, data_offset + physical + 0x8000u) != value)
                        return false;
                }
            }
            const uint8_t type = reader.read(reader.context, canonical + 0x7FD6u);
            candidate = {SnesRomMap::Fx3, size, canonical,
                         header_ram_size(reader, canonical + 0x7FC0u, type), type};
            candidate_score = fx_score + 16;
            return true;
        }
        if (trailer)
            return false; // The erased trailer is specific to a validated FX3 dump.

        if (data_size > 4u * MIB) {
            const int exlo_low = header_score(reader, data_offset + 0x7FC0u,
                                              SnesRomMap::ExLoRom);
            const int exlo_high = header_score(reader, data_offset + 0x407FC0u,
                                               SnesRomMap::ExLoRom);
            const int exhi = data_size >= 0x410000u ?
                header_score(reader, data_offset + 0x40FFC0u, SnesRomMap::ExHiRom) : -1;
            const int exlo = exlo_low > exlo_high ? exlo_low : exlo_high;
            candidate_score = exhi > exlo ? exhi : exlo;
            if (candidate_score < 8)
                return false;
            const bool use_exhi = exhi > exlo;
            const uint32_t header = data_offset + (use_exhi ? 0x40FFC0u :
                (exlo_high > exlo_low ? 0x407FC0u : 0x7FC0u));
            const uint8_t type = reader.read(reader.context, header + 0x16u);
            candidate = {use_exhi ? SnesRomMap::ExHiRom : SnesRomMap::ExLoRom,
                         data_size, data_offset, header_ram_size(reader, header, type), type};
            return true;
        }

        const int lo_score = header_score(reader, data_offset + 0x7FC0u,
                                          SnesRomMap::LoRom);
        const int hi_score = data_size >= 0x10000u ?
            header_score(reader, data_offset + 0xFFC0u, SnesRomMap::HiRom) : -1;
        candidate_score = hi_score > lo_score ? hi_score : lo_score;
        if (candidate_score < 8)
            return false;
        const bool use_hirom = hi_score > lo_score;
        const uint32_t header = data_offset + (use_hirom ? 0xFFC0u : 0x7FC0u);
        const uint8_t type = reader.read(reader.context, header + 0x16u);
        candidate = {use_hirom ? SnesRomMap::HiRom : SnesRomMap::LoRom,
                     data_size, data_offset, header_ram_size(reader, header, type), type};
        return true;
    };

    SnesRomInfo unheadered{}, headered{};
    int unheadered_score = -1;
    int headered_score = -1;
    const bool has_unheadered = detect_at(0, unheadered, unheadered_score);
    const bool has_headered = file_size > 512u &&
        detect_at(512u, headered, headered_score);
    if (has_unheadered == has_headered) {
        if (!has_unheadered || unheadered_score == headered_score)
            return false;
        info = headered_score > unheadered_score ? headered : unheadered;
        return true;
    }
    info = has_headered ? headered : unheadered;
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
    if (info.map == SnesRomMap::Fx3) {
        const uint32_t mirrored_bank = bank & 0x7Fu;
        source_offset = mirrored_bank < 0x40u ?
            (mirrored_bank << 15) | (address & 0x7FFFu) :
            ((mirrored_bank - 0x40u) << 16) | address;
        source_offset = mirror_offset(info.size, source_offset);
        return true;
    }
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
    const uint32_t attributes = DESCRIPTOR_EXTENDED |
        (static_cast<uint32_t>(info.cartridge_type) << 16) |
        (static_cast<uint32_t>(ram_size_code(info.ram_size)) << 8) |
        static_cast<uint32_t>(info.map);
    const uint32_t words[] = {0x504D3353u, attributes, info.size,
                              ~(0x504D3353u ^ attributes ^ info.size)};
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t byte = 0; byte < 4; ++byte)
            bytes[i * 4u + byte] = static_cast<uint8_t>(words[i] >> (byte * 8u));
    }
}

SnesRomInfo snes_rom_installed_info(const SnesRomReader& physical) {
    SnesRomInfo info {SnesRomMap::Fx3Physical, 0, 0};
    if (!physical.read)
        return info;
    uint32_t words[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t byte = 0; byte < 4; ++byte)
            words[i] |= static_cast<uint32_t>(physical.read(physical.context,
                SNES_ROM_DESCRIPTOR_ADDRESS + i * 4u + byte)) << (byte * 8u);
    }
    const bool extended = (words[1] & DESCRIPTOR_EXTENDED) != 0;
    const uint32_t map = extended ? words[1] & 0xFFu : words[1];
    const uint8_t ram_code = static_cast<uint8_t>(words[1] >> 8);
    if (words[0] == 0x504D3353u &&
        (map <= static_cast<uint32_t>(SnesRomMap::ExHiRom) ||
         map == static_cast<uint32_t>(SnesRomMap::Fx3)) &&
        words[2] && words[2] <= 8u * MIB && (words[2] % (32u * KIB)) == 0 &&
        (map != static_cast<uint32_t>(SnesRomMap::Fx3) || words[2] <= 4u * MIB) &&
        (!extended || ram_code <= 7u) &&
        words[3] == ~(words[0] ^ words[1] ^ words[2])) {
        info.map = static_cast<SnesRomMap>(map);
        info.size = words[2];
        if (extended) {
            info.ram_size = ram_code ? 1u << (ram_code + 10u) : 0;
            info.cartridge_type = static_cast<uint8_t>(words[1] >> 16);
        }
        return info;
    }

    // Descriptor-less and smaller devices retain their physical map, but a
    // validated bus-visible header can still describe SRAM and persistence.
    const uint32_t headers[] = {0x00FFC0u, 0x40FFC0u};
    for (uint32_t header : headers) {
        const int lo = header_score(physical, header, SnesRomMap::LoRom);
        const int hi = header_score(physical, header, SnesRomMap::HiRom);
        if (lo < 8 && hi < 8)
            continue;
        const uint8_t type = physical.read(physical.context, header + 0x16u);
        info.ram_size = header_ram_size(physical, header, type);
        info.cartridge_type = type;
        break;
    }
    return info;
}

SnesRomMap snes_rom_installed_map(const SnesRomReader& physical) {
    return snes_rom_installed_info(physical).map;
}

bool snes_rom_has_persistent_ram(const SnesRomInfo& info) {
    if (!info.ram_size)
        return false;
    if (info.cartridge_type == SNES_CARTRIDGE_FX3)
        return false;
    if (info.cartridge_type == SNES_CARTRIDGE_FX3_BATTERY)
        return true;
    const uint8_t feature = info.cartridge_type & 0x0Fu;
    return feature == 0x02u || feature == 0x05u;
}
