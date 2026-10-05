/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <atomic>
#include "audio/fx3_audio_stream.h"

enum class Fx3VideoStatus : uint8_t { Idle, Opening, Streaming, Eof, Error };
enum class Fx3VideoError : uint8_t {
    None, Unavailable, Busy, OpenFailed, ReadFailed, InvalidFormat, BadCrc,
};

namespace fx3_video {
constexpr uint16_t MMIO_BASE = 0x7F40;
constexpr uint16_t MMIO_END = 0x7F5F;
constexpr uint32_t RAM_OFFSET = 0x10000; // Bank $71; bank $70 remains untouched.
constexpr uint32_t PAGE_SIZE = 0x8000;
constexpr uint32_t MAP_SIZE = 32u * 28u * 2u;
constexpr uint32_t PALETTE_SIZE = 32;
constexpr uint32_t MAX_TILES = 896;
constexpr uint8_t NO_PAGE = 0xFF;
enum Register : uint8_t {
    Command, AssetIdLow, AssetIdHigh, Status, Error, Page, ActivePage, Flags,
    Pts = 0x08, TileBytes = 0x0C, FrameBytes = 0x0E,
    Duration = 0x10, FrameCount = 0x14, Version = 0x18,
    FrameDuration = 0x1C,
};
// Stop preserves ACTIVE and the GSU lock. Release requires host DMA to be disabled.
enum CommandValue : uint8_t { Stop, Play, Release };
}

// Core 1 stages two immutable DMA pages in existing GSU RAM, not new frame buffers.
void fx3_video_init(const Fx3AudioSource& source, std::atomic<uint8_t>* ram,
                    bool (*acquire)(), void (*release)());
void fx3_video_task();
void fx3_video_request_reset();
bool fx3_video_gsu_locked();
bool fx3_video_mmio_address(uint32_t address);
uint8_t fx3_video_host_read(uint16_t address);
void fx3_video_host_write(uint16_t address, uint8_t value);
