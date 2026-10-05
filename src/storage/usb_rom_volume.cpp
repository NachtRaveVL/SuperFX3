/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "usb_rom_volume.h"

#include <string.h>

namespace {
constexpr char README_TEXT[] =
    "SuperFX3 USB ROM loader\r\n"
    "\r\n"
    "Copy one LoROM/HiROM/ExLoROM/ExHiROM/FX3 .sfc/.smc,\r\n"
    "or a full-device .rom/.bin physical bus image.\r\n"
    "After copying, safely EJECT the drive to install. Keep USB power on\r\n"
    "until programming finishes and /O_IRQ is released.\r\n"
    "Keep the cartridge out of a powered SNES while this drive is mounted.\r\n"
    "FX3 updates parallel NOR and QSPI FX ROM; other maps update only NOR.\r\n"
    "An interrupted installation requires re-uploading the ROM.\r\n";

void put16(uint8_t* data, size_t offset, uint16_t value) {
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t* data, size_t offset, uint32_t value) {
    put16(data, offset, static_cast<uint16_t>(value));
    put16(data, offset + 2, static_cast<uint16_t>(value >> 16));
}

uint16_t get16(const uint8_t* data, size_t offset) {
    return static_cast<uint16_t>(data[offset]) |
           (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t get32(const uint8_t* data, size_t offset) {
    return static_cast<uint32_t>(get16(data, offset)) |
           (static_cast<uint32_t>(get16(data, offset + 2)) << 16);
}
}

UsbRomVolume::UsbRomVolume(const UsbRomSink& sink) : sink_(sink) {
    reset();
}

void UsbRomVolume::set_capacity(uint32_t capacity) {
    capacity_ = capacity;
    staging_pages_ = capacity / PAGE_SIZE + 4u;
    block_count_ = FIRST_UPLOAD_BLOCK + staging_pages_ * SECTORS_PER_CLUSTER;
    fat12_ = staging_pages_ + 1u < 4085u;
    reset();
}

void UsbRomVolume::reset() {
    memset(fat_, 0, sizeof(fat_));
    memset(root_, 0, sizeof(root_));
    memset(received_, 0, sizeof(received_));
    if (fat12_) {
        fat_[0] = 0xF8;
        fat_[1] = 0xFF;
        fat_[2] = 0xFF;
        set_fat_entry(README_CLUSTER, 0x0FFFu);
    } else {
        put16(fat_, 0, 0xFFF8);
        put16(fat_, 2, 0xFFFF);
        set_fat_entry(README_CLUSTER, 0xFFFFu);
    }

    // Volume label and a one-cluster readme leave cluster 3 as the first free cluster.
    memcpy(root_, "SUPERFX3    ", 11);
    root_[11] = 0x08;
    memcpy(root_ + 32, "README  TXT", 11);
    root_[32 + 11] = 0x21;
    put16(root_ + 32, 26, README_CLUSTER);
    put32(root_ + 32, 28, static_cast<uint32_t>(sizeof(README_TEXT) - 1u));

    sink_started_ = false;
    completion_sent_ = false;
}

void UsbRomVolume::set_fat_entry(uint16_t cluster, uint16_t value) {
    if (!fat12_) {
        put16(fat_, static_cast<size_t>(cluster) * 2u, value);
        return;
    }
    const size_t offset = cluster + cluster / 2u;
    if (cluster & 1u) {
        fat_[offset] = static_cast<uint8_t>((fat_[offset] & 0x0Fu) | (value << 4));
        fat_[offset + 1u] = static_cast<uint8_t>(value >> 4);
    } else {
        fat_[offset] = static_cast<uint8_t>(value);
        fat_[offset + 1u] = static_cast<uint8_t>((fat_[offset + 1u] & 0xF0u) |
                                                 ((value >> 8) & 0x0Fu));
    }
}

uint16_t UsbRomVolume::fat_entry(uint16_t cluster) const {
    const size_t offset = fat12_ ? cluster + cluster / 2u :
        static_cast<size_t>(cluster) * 2u;
    if (offset + 1u >= sizeof(fat_))
        return 0;
    const uint16_t value = get16(fat_, offset);
    return fat12_ ? static_cast<uint16_t>((cluster & 1u ? value >> 4 : value) & 0x0FFFu) :
        value;
}

bool UsbRomVolume::flush() {
    return !sink_started_ || (sink_.flush && sink_.flush(sink_.context));
}

bool UsbRomVolume::eject() {
    if (completion_sent_)
        return true;
    const uint8_t* selected = nullptr;
    for (size_t offset = 0; offset + 32u <= sizeof(root_); offset += 32u) {
        const uint8_t* entry = root_ + offset;
        if (entry[0] == 0)
            break;
        if (entry[0] == 0xE5 || entry[11] == 0x0F ||
            (entry[11] & 0x18u) != 0)
            continue;
        const bool sfc = memcmp(entry + 8, "SFC", 3) == 0;
        const bool smc = memcmp(entry + 8, "SMC", 3) == 0;
        const bool bin = memcmp(entry + 8, "BIN", 3) == 0;
        const bool rom = memcmp(entry + 8, "ROM", 3) == 0;
        if (!sfc && !smc && !bin && !rom)
            continue;

        if (selected)
            return false; // One ROM per mount; do not guess between two candidates.
        selected = entry;
    }
    if (!selected || !sink_started_ || !sink_.complete)
        return false;

    const uint32_t size = get32(selected, 28);
    if (!size || size > capacity_ + MAX_SOURCE_OVERHEAD)
        return false;
    const uint32_t count = (size + PAGE_SIZE - 1u) / PAGE_SIZE;
    uint16_t cluster = get16(selected, 26);
    uint32_t remaining = size;
    for (uint32_t page = 0; page < count; ++page) {
        if (cluster < 3 || cluster >= 3u + staging_pages_)
            return false;
        const uint16_t physical = static_cast<uint16_t>(cluster - 3u);
        for (uint32_t previous = 0; previous < page; ++previous) {
            if (pages_[previous] == physical)
                return false; // Cyclic FAT chain.
        }
        pages_[page] = physical;
        const uint32_t bytes = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;
        const uint32_t sectors = (bytes + BLOCK_SIZE - 1u) / BLOCK_SIZE;
        const uint8_t mask = static_cast<uint8_t>((1u << sectors) - 1u);
        if ((received_[physical] & mask) != mask)
            return false;
        remaining -= bytes;
        cluster = fat_entry(cluster);
    }
    if (cluster < (fat12_ ? 0x0FF8u : 0xFFF8u) || !flush())
        return false;
    const UsbRomFileType type = memcmp(selected + 8, "SMC", 3) == 0 ?
        UsbRomFileType::Smc : (memcmp(selected + 8, "SFC", 3) == 0 ?
        UsbRomFileType::Sfc : UsbRomFileType::Raw);
    completion_sent_ = sink_.complete(sink_.context, type, size, pages_, count);
    return completion_sent_;
}

bool UsbRomVolume::read(uint32_t lba, uint8_t* data, size_t size) const {
    if (!data || size != BLOCK_SIZE || lba >= block_count_)
        return false;
    memset(data, 0, size);
    if (lba == 0) {
        data[0] = 0xEB; data[1] = 0x3C; data[2] = 0x90;
        memcpy(data + 3, "SUPERFX3", 8);
        put16(data, 11, BLOCK_SIZE);
        data[13] = SECTORS_PER_CLUSTER;
        put16(data, 14, 1);
        data[16] = 2;
        put16(data, 17, 256);
        put16(data, 19, 0);
        data[21] = 0xF8;
        put16(data, 22, FAT_SECTORS);
        put16(data, 24, 32);
        put16(data, 26, 64);
        put32(data, 32, block_count_);
        data[36] = 0x80; data[38] = 0x29;
        put32(data, 39, 0x53335833u);
        memcpy(data + 43, "SUPERFX3   ", 11);
        memcpy(data + 54, fat12_ ? "FAT12   " : "FAT16   ", 8);
        data[510] = 0x55; data[511] = 0xAA;
        return true;
    }
    if (lba >= FAT1_START && lba < FAT1_START + FAT_SECTORS) {
        memcpy(data, fat_ + (lba - FAT1_START) * BLOCK_SIZE, BLOCK_SIZE);
        return true;
    }
    if (lba >= FAT2_START && lba < FAT2_START + FAT_SECTORS) {
        memcpy(data, fat_ + (lba - FAT2_START) * BLOCK_SIZE, BLOCK_SIZE);
        return true;
    }
    if (lba >= ROOT_START && lba < ROOT_START + ROOT_SECTORS) {
        memcpy(data, root_ + (lba - ROOT_START) * BLOCK_SIZE, BLOCK_SIZE);
        return true;
    }
    if (lba >= DATA_START) {
        const uint32_t relative = lba - DATA_START;
        const uint16_t cluster = static_cast<uint16_t>(relative / SECTORS_PER_CLUSTER + 2u);
        const uint32_t within = (relative % SECTORS_PER_CLUSTER) * BLOCK_SIZE;
        if (cluster == README_CLUSTER) {
            if (within < sizeof(README_TEXT) - 1u) {
                const size_t count = (sizeof(README_TEXT) - 1u - within) < BLOCK_SIZE ?
                    sizeof(README_TEXT) - 1u - within : BLOCK_SIZE;
                memcpy(data, README_TEXT + within, count);
            }
            return true;
        }
        if (sink_started_ && sink_.read && cluster >= 3) {
            const uint32_t file_offset = (static_cast<uint32_t>(cluster) - 3u) * PAGE_SIZE + within;
            for (uint32_t index = 0; index < BLOCK_SIZE; ++index)
                data[index] = sink_.read(sink_.context, file_offset + index);
        }
        return true;
    }
    return true;
}

bool UsbRomVolume::write(uint32_t lba, const uint8_t* data, size_t size) {
    if (!data || size != BLOCK_SIZE || lba >= block_count_ || completion_sent_)
        return false;
    if (lba >= FAT1_START && lba < FAT1_START + FAT_SECTORS) {
        memcpy(fat_ + (lba - FAT1_START) * BLOCK_SIZE, data, BLOCK_SIZE);
        return true;
    }
    if (lba >= FAT2_START && lba < FAT2_START + FAT_SECTORS) {
        memcpy(fat_ + (lba - FAT2_START) * BLOCK_SIZE, data, BLOCK_SIZE);
        return true;
    }
    if (lba >= ROOT_START && lba < ROOT_START + ROOT_SECTORS) {
        memcpy(root_ + (lba - ROOT_START) * BLOCK_SIZE, data, BLOCK_SIZE);
        return true;
    }
    if (lba < FIRST_UPLOAD_BLOCK)
        return true;
    if (!sink_started_) {
        sink_started_ = sink_.begin && sink_.begin(sink_.context, UsbRomFileType::Raw, 3);
        if (!sink_started_)
            return false;
    }
    const uint32_t block = lba - FIRST_UPLOAD_BLOCK;
    const uint32_t file_offset = block * BLOCK_SIZE;
    if (!sink_.write ||
        !sink_.write(sink_.context, file_offset, data, size))
        return false;
    received_[block / 8u] |= static_cast<uint8_t>(1u << (block % 8u));
    return true;
}
