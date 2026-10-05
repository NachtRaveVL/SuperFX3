/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

#ifndef SUPERFX3_AUDIO_SD
#define SUPERFX3_AUDIO_SD 0
#endif

enum class Fx3AudioCommand : uint8_t {
    Stop,
    Play,
    Pause,
    Resume,
};

enum class Fx3AudioStatus : uint8_t {
    Idle,
    Opening,
    Priming,
    Playing,
    Draining,
    Eof,
    Error,
    Underrun,
    Paused,
};

enum class Fx3AudioError : uint8_t {
    None,
    Unavailable,
    OpenFailed,
    ReadFailed,
    PartialBlock,
    Busy,
    InvalidCommand,
    CommandQueueFull,
    FifoUnderrun,
};

enum class Fx3AudioEvent : uint8_t {
    PageMissed = 1,
    PacketMissed,
    SpcUnderrun,
    SpcOverrun,
};

struct Fx3AudioSource {
    void* context = nullptr;
    bool (*open)(void* context, uint16_t asset_id) = nullptr;
    // Bounded SRAM-buffer read: bytes read, zero at EOF, -1 on error, -2 to retry.
    // Shared-QSPI backends must keep the complete live Core-0 path XIP-safe.
    int32_t (*read)(void* context, uint8_t* data, uint32_t size) = nullptr;
    void (*close)(void* context) = nullptr;
};

namespace fx3_audio {
constexpr uint16_t HDMA_BASE = 0x6000;
constexpr uint16_t HDMA_END = 0x6FFF;
constexpr uint16_t HDMA_EMPTY = HDMA_END;
constexpr uint16_t MMIO_BASE = 0x7F00;
constexpr uint16_t MMIO_END = 0x7F33;
constexpr uint32_t FIFO_SIZE = 32u * 1024u;
constexpr uint32_t FIFO_LOW_WATER = 8u * 1024u;
constexpr uint32_t FIFO_HIGH_WATER = 24u * 1024u;
constexpr uint32_t HDMA_PAGE_COUNT = 4;
constexpr uint32_t HDMA_PAGE_SIZE = 1024;
constexpr uint8_t NO_PAGE = 0xFF;
constexpr uint8_t PAL_RATE = 1; // Flags bit 0: 50 Hz HDMA cadence, still 16 kHz BRR.

enum Register : uint8_t {
    Command = 0x00,
    AssetIdLow = 0x01,
    AssetIdHigh = 0x02,
    Flags = 0x03,
    Status = 0x04,
    Error = 0x05,
    HdmaPage = 0x06,
    HdmaSequence = 0x07,
    SpcLevel = 0x08,
    Event = 0x09,
    HdmaLastSequence = 0x0A,
    CounterBase = 0x10,
};

enum Counter : uint8_t {
    SdReads,
    SdBytes,
    PicoFifoLowWater,
    HdmaPagesBuilt,
    HdmaPagesMissed,
    StreamPacketsSent,
    StreamPacketsMissed,
    SpcUnderruns,
    SpcOverruns,
    CounterCount,
};
}

// Initialize before either core services the cartridge bus.
void fx3_audio_init(const Fx3AudioSource& source);
void fx3_audio_task();
void fx3_audio_request_reset();

bool fx3_audio_mmio_address(uint32_t address);
bool fx3_audio_hdma_address(uint32_t address);
uint8_t fx3_audio_host_read(uint16_t address);
// At VBlank, report queued SPC segments, then claim one page (or release with NO_PAGE).
// Never reuse an ACTIVE table next frame; use HDMA_EMPTY when no page is offered.
void fx3_audio_host_write(uint16_t address, uint8_t value);
uint8_t fx3_audio_hdma_read(uint16_t address);
