/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "sd_audio_source.h"
#include "qspi_sd.h"
#include "fx_sync.h"
#include "audio/fx3_audio_stream.h"

#include <cstring>
#include "pico/stdlib.h"

#if SUPERFX3_AUDIO_SD
namespace {
uint8_t g_sector[514] {};
uint32_t g_cached_sector = UINT32_MAX;
uint32_t g_card_sectors = 0;
bool g_block_addressed = false;
bool g_mounted = false;
bool g_open[2] {};
uint8_t g_read_errors = 0;

struct Volume {
    uint32_t fat = 0, fat_sectors = 0, root = 0, root_sectors = 0;
    uint32_t data = 0, clusters = 0;
    uint8_t sectors_per_cluster = 0;
    bool fat32 = false;
} g_volume;

struct File {
    uint32_t cluster = 0, size = 0, position = 0, cluster_position = 0;
    uint32_t anchor = 0, power = 1, steps = 0;
} g_files[2];

enum class ReadResult : uint8_t { Ok, Retry, Error };

uint16_t le16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
}
uint32_t le32(const uint8_t* data) {
    return data[0] | (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}
uint16_t crc16(const uint8_t* data, uint32_t size) {
    uint16_t crc = 0;
    for (uint32_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = static_cast<uint16_t>((crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0u));
    }
    return crc;
}

QspiSdResult command(uint8_t number, uint32_t argument, uint8_t* response,
                     uint32_t response_size = 1, uint32_t block_size = 0,
                     uint32_t clock_hz = 4000000u) {
    uint8_t packet[6] {static_cast<uint8_t>(0x40u | number),
        static_cast<uint8_t>(argument >> 24), static_cast<uint8_t>(argument >> 16),
        static_cast<uint8_t>(argument >> 8), static_cast<uint8_t>(argument), 0};
    uint8_t crc = 0;
    for (uint8_t i = 0; i < 5; ++i) {
        uint8_t value = packet[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = static_cast<uint8_t>(crc << 1);
            if ((value ^ crc) & 0x80u) crc ^= 0x09u;
            value = static_cast<uint8_t>(value << 1);
        }
    }
    packet[5] = static_cast<uint8_t>((crc << 1) | 1u);
    return qspi_sd_command(packet, response, response_size,
                           block_size ? g_sector : nullptr, block_size, clock_hz);
}

bool valid_crc(uint32_t size) {
    return crc16(g_sector, size) ==
        static_cast<uint16_t>((static_cast<uint16_t>(g_sector[size]) << 8) | g_sector[size + 1u]);
}

bool initialize_card() {
    uint8_t response[5] {};
    if (qspi_sd_exchange(nullptr, nullptr, 10, 250000u, false) != QspiSdResult::Ok ||
        command(0, 0, response, 1, 0, 250000u) != QspiSdResult::Ok || response[0] != 1)
        return false;
    if (command(8, 0x1AA, response, 5, 0, 250000u) != QspiSdResult::Ok)
        return false;
    const bool version2 = response[0] == 1;
    if ((version2 && (response[1] || response[2] || response[3] != 1 || response[4] != 0xAA)) ||
        (!version2 && response[0] != 5))
        return false;
    const uint32_t start = time_us_32();
    do {
        if (command(55, 0, response, 1, 0, 250000u) != QspiSdResult::Ok || response[0] > 1 ||
            command(41, version2 ? 0x40000000u : 0, response, 1, 0, 250000u) !=
                QspiSdResult::Ok || response[0] > 1)
            return false;
        if (!response[0]) break;
        // XIP is restored here; keep guest execution moving during slow card startup.
        fx_sync_core1_service();
    } while (static_cast<uint32_t>(time_us_32() - start) < 1000000u);
    if (response[0] || command(58, 0, response, 5, 0, 250000u) != QspiSdResult::Ok ||
        response[0] || !(response[1] & 0x80u) || !(response[2] & 0x30u))
        return false;
    g_block_addressed = version2 && (response[1] & 0x40u);
    if (!g_block_addressed && (command(16, 512, response) != QspiSdResult::Ok || response[0]))
        return false;
    if (command(9, 0, response, 1, 16) != QspiSdResult::Ok || response[0] || !valid_crc(16))
        return false;
    uint64_t sectors = 0;
    if ((g_sector[0] >> 6) == 1) {
        const uint32_t size = (static_cast<uint32_t>(g_sector[7] & 0x3Fu) << 16) |
            (static_cast<uint32_t>(g_sector[8]) << 8) | g_sector[9];
        sectors = (static_cast<uint64_t>(size) + 1u) * 1024u;
    } else if ((g_sector[0] >> 6) == 0) {
        if ((g_sector[5] & 15u) < 9 || (g_sector[5] & 15u) > 11)
            return false;
        const uint32_t size = (static_cast<uint32_t>(g_sector[6] & 3u) << 10) |
            (static_cast<uint32_t>(g_sector[7]) << 2) | (g_sector[8] >> 6);
        const uint8_t shift = static_cast<uint8_t>(((g_sector[9] & 3u) << 1) |
                                                  (g_sector[10] >> 7));
        sectors = ((static_cast<uint64_t>(size) + 1u) << (shift + 2u + (g_sector[5] & 15u))) / 512u;
    }
    if (!sectors || sectors > UINT32_MAX || (!g_block_addressed && sectors > 0x800000u))
        return false;
    g_card_sectors = static_cast<uint32_t>(sectors);
    return true;
}

ReadResult read_sector(uint32_t sector) {
    if (sector >= g_card_sectors)
        return ReadResult::Error;
    if (g_cached_sector == sector)
        return ReadResult::Ok;
    g_cached_sector = UINT32_MAX;
    uint8_t response = 0xFF;
    const QspiSdResult result = command(17, g_block_addressed ? sector : sector * 512u,
                                        &response, 1, 512);
    if (result == QspiSdResult::Busy || result == QspiSdResult::Cancelled)
        return ReadResult::Retry;
    if (result == QspiSdResult::Timeout || (result == QspiSdResult::Ok && !response && !valid_crc(512)))
        return ++g_read_errors < 3 ? ReadResult::Retry : ReadResult::Error;
    if (result != QspiSdResult::Ok || response)
        return ReadResult::Error;
    g_read_errors = 0;
    g_cached_sector = sector;
    return ReadResult::Ok;
}

bool valid_cluster(uint32_t cluster) {
    return cluster >= 2 && cluster - 2u < g_volume.clusters;
}

bool parse_volume(uint32_t start, uint32_t limit) {
    if (g_sector[510] != 0x55 || g_sector[511] != 0xAA || le16(g_sector + 11) != 512)
        return false;
    const uint8_t cluster_sectors = g_sector[13];
    const uint32_t reserved = le16(g_sector + 14);
    const uint32_t fats = g_sector[16];
    const uint32_t root_entries = le16(g_sector + 17);
    const uint32_t total = le16(g_sector + 19) ? le16(g_sector + 19) : le32(g_sector + 32);
    const uint32_t fat_sectors = le16(g_sector + 22) ? le16(g_sector + 22) : le32(g_sector + 36);
    const uint32_t root_sectors = (root_entries * 32u + 511u) / 512u;
    const uint64_t overhead = reserved + static_cast<uint64_t>(fats) * fat_sectors + root_sectors;
    if (!cluster_sectors || cluster_sectors > 128 || (cluster_sectors & (cluster_sectors - 1u)) ||
        !reserved || !fats || fats > 2 || !fat_sectors || overhead >= total ||
        total > limit || start >= g_card_sectors || total > g_card_sectors - start)
        return false;
    const uint32_t clusters = (total - static_cast<uint32_t>(overhead)) / cluster_sectors;
    const bool fat32 = clusters >= 65525u;
    if (clusters < 4085u || clusters >= 0x0FFFFFF5u ||
        (fat32 ? root_entries || le16(g_sector + 22) || le16(g_sector + 42) : !root_entries) ||
        static_cast<uint64_t>(clusters + 2u) * (fat32 ? 4u : 2u) >
            static_cast<uint64_t>(fat_sectors) * 512u)
        return false;
    uint32_t active_fat = 0;
    if (fat32 && (le16(g_sector + 40) & 0x80u)) {
        active_fat = le16(g_sector + 40) & 15u;
        if (active_fat >= fats) return false;
    }
    g_volume = {start + reserved + active_fat * fat_sectors, fat_sectors,
        fat32 ? le32(g_sector + 44) & 0x0FFFFFFFu : start + reserved + fats * fat_sectors,
        root_sectors, start + static_cast<uint32_t>(overhead), clusters, cluster_sectors, fat32};
    return !fat32 || valid_cluster(g_volume.root);
}

ReadResult next_cluster(uint32_t cluster, uint32_t& next) {
    if (!valid_cluster(cluster)) return ReadResult::Error;
    const uint32_t offset = cluster * (g_volume.fat32 ? 4u : 2u);
    const ReadResult result = read_sector(g_volume.fat + offset / 512u);
    if (result != ReadResult::Ok) return result;
    next = g_volume.fat32 ? le32(g_sector + offset % 512u) & 0x0FFFFFFFu :
                           le16(g_sector + offset % 512u);
    return ReadResult::Ok;
}

bool find_entry(uint32_t cluster, bool fixed_root, const char* name, bool directory,
                uint32_t& found_cluster, uint32_t& size) {
    uint32_t visited = 0;
    do {
        const uint32_t count = fixed_root ? g_volume.root_sectors : g_volume.sectors_per_cluster;
        if (!fixed_root && !valid_cluster(cluster)) return false;
        const uint32_t first = fixed_root ? g_volume.root :
            g_volume.data + (cluster - 2u) * g_volume.sectors_per_cluster;
        for (uint32_t sector = 0; sector < count; ++sector) {
            if (++visited > 256u || read_sector(first + sector) != ReadResult::Ok)
                return false;
            for (uint32_t offset = 0; offset < 512; offset += 32) {
                const uint8_t* entry = g_sector + offset;
                if (!entry[0]) return false;
                if (entry[0] == 0xE5 || (entry[11] & 0x08u) ||
                    (bool(entry[11] & 0x10u) != directory) || std::memcmp(entry, name, 11))
                    continue;
                found_cluster = le16(entry + 26) |
                    (g_volume.fat32 ? static_cast<uint32_t>(le16(entry + 20)) << 16 : 0);
                size = le32(entry + 28);
                return valid_cluster(found_cluster) || (!directory && !size);
            }
        }
        uint32_t next = 0;
        if (fixed_root || next_cluster(cluster, next) != ReadResult::Ok || next == cluster)
            return false;
        cluster = next;
    } while (valid_cluster(cluster));
    return false;
}
}
#endif

bool sd_audio_mount() {
#if SUPERFX3_AUDIO_SD
    if (g_mounted && g_open[1]) return true;
    g_mounted = g_open[0] = g_open[1] = false;
    g_cached_sector = UINT32_MAX;
    g_read_errors = 0;
    if (!initialize_card() || read_sector(0) != ReadResult::Ok)
        return false;
    if (!parse_volume(0, g_card_sectors)) {
        uint32_t starts[4] {}, sizes[4] {};
        if (g_sector[510] != 0x55 || g_sector[511] != 0xAA) return false;
        for (uint32_t i = 0; i < 4; ++i) {
            const uint8_t* entry = g_sector + 446u + i * 16u;
            const uint8_t type = entry[4];
            if (type == 0x04 || type == 0x06 || type == 0x0E || type == 0x0B || type == 0x0C) {
                starts[i] = le32(entry + 8);
                sizes[i] = le32(entry + 12);
            }
        }
        bool found = false;
        for (uint32_t i = 0; i < 4 && !found; ++i)
            if (starts[i] && starts[i] < g_card_sectors && sizes[i] &&
                sizes[i] <= g_card_sectors - starts[i] &&
                read_sector(starts[i]) == ReadResult::Ok)
                found = parse_volume(starts[i], sizes[i]);
        if (!found) return false;
    }
    g_mounted = true;
    return true;
#else
    return false;
#endif
}

static bool open_file(const char* path, uint8_t slot) {
#if SUPERFX3_AUDIO_SD
    bool& g_open = ::g_open[slot];
    File& g_file = g_files[slot];
    g_open = false;
    if (!g_mounted || !path || std::strlen(path) != 15 || std::memcmp(path, slot ? "/video/" : "/audio/", 7))
        return false;
    char name[11] {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 'B', 'R', 'R'};
    for (uint32_t i = 0; i < 4; ++i) {
        const char value = path[7u + i];
        if (!((value >= '0' && value <= '9') || (value >= 'A' && value <= 'F')))
            return false;
        name[i] = value;
    }
    if (std::memcmp(path + 11, slot ? ".fmv" : ".brr", 4)) return false;
    if (slot) { name[8] = 'F'; name[9] = 'M'; name[10] = 'V'; }
    uint32_t audio = 0, cluster = 0, size = 0;
    if (!find_entry(g_volume.root, !g_volume.fat32, slot ? "VIDEO      " : "AUDIO      ", true, audio, size) ||
        !find_entry(audio, false, name, false, cluster, size) ||
        static_cast<uint64_t>(size) > static_cast<uint64_t>(g_volume.clusters) *
            g_volume.sectors_per_cluster * 512u)
        return false;
    g_file = {cluster, size, 0, 0, cluster, 1, 0};
    g_open = true;
    return true;
#else
    (void)path; (void)slot;
    return false;
#endif
}

static int32_t read_file(uint8_t* data, uint32_t size, uint8_t slot) {
#if SUPERFX3_AUDIO_SD
    bool& g_open = ::g_open[slot];
    File& g_file = g_files[slot];
    if (!g_mounted || !g_open || !data || !size || size > 4096u) return -1;
    uint32_t copied = 0;
    while (copied < size && g_file.position < g_file.size) {
        const uint32_t cluster_bytes = static_cast<uint32_t>(g_volume.sectors_per_cluster) * 512u;
        ReadResult result = ReadResult::Ok;
        if (g_file.cluster_position == cluster_bytes) {
            uint32_t next = 0;
            result = next_cluster(g_file.cluster, next);
            if (result == ReadResult::Ok) {
                if (!valid_cluster(next) || next == g_file.cluster || next == g_file.anchor)
                    result = ReadResult::Error;
                else {
                    g_file.cluster = next;
                    g_file.cluster_position = 0;
                    if (++g_file.steps == g_file.power) {
                        g_file.anchor = next;
                        g_file.steps = 0;
                        g_file.power *= 2u;
                    }
                }
            }
        }
        if (result == ReadResult::Ok)
            result = read_sector(g_volume.data + (g_file.cluster - 2u) *
                g_volume.sectors_per_cluster + g_file.cluster_position / 512u);
        if (result != ReadResult::Ok) {
            if (result == ReadResult::Error) g_open = g_mounted = false;
            return copied ? static_cast<int32_t>(copied) : result == ReadResult::Retry ? -2 : -1;
        }
        const uint32_t offset = g_file.cluster_position % 512u;
        uint32_t count = 512u - offset;
        if (count > size - copied) count = size - copied;
        if (count > g_file.size - g_file.position) count = g_file.size - g_file.position;
        std::memcpy(data + copied, g_sector + offset, count);
        copied += count;
        g_file.position += count;
        g_file.cluster_position += count;
    }
    return static_cast<int32_t>(copied);
#else
    (void)data; (void)size; (void)slot;
    return -1;
#endif
}

void sd_audio_close() {
#if SUPERFX3_AUDIO_SD
    g_open[0] = false;
    g_cached_sector = UINT32_MAX;
#endif
}

bool sd_audio_open(const char* path) { return open_file(path, 0); }
int32_t sd_audio_read(uint8_t* data, uint32_t size) { return read_file(data, size, 0); }
bool sd_video_open(const char* path) { return open_file(path, 1); }
bool sd_video_mount() {
#if SUPERFX3_AUDIO_SD
    if (g_mounted && (g_open[0] || g_open[1])) return true;
#endif
    return sd_audio_mount();
}
int32_t sd_video_read(uint8_t* data, uint32_t size) { return read_file(data, size, 1); }
void sd_video_close() {
#if SUPERFX3_AUDIO_SD
    g_open[1] = false;
    g_cached_sector = UINT32_MAX;
#endif
}
