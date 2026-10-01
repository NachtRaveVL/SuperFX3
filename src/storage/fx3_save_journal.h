/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fx3_qspi_layout.h"

namespace fx3_save {

constexpr uint32_t PAYLOAD_SIZE = 128u * 1024u;
constexpr uint32_t HEADER_SIZE = fx3_qspi::FLASH_PAGE_SIZE;
constexpr uint32_t SLOT_SIZE =
    ((HEADER_SIZE + PAYLOAD_SIZE + fx3_qspi::FLASH_SECTOR_SIZE - 1u) /
     fx3_qspi::FLASH_SECTOR_SIZE) * fx3_qspi::FLASH_SECTOR_SIZE;
constexpr uint32_t SLOT_COUNT = fx3_qspi::SAVE_SIZE / SLOT_SIZE;

static_assert((PAYLOAD_SIZE & (fx3_qspi::FLASH_PAGE_SIZE - 1u)) == 0,
              "The FX3 save snapshot must be flash-page aligned.");
static_assert(SLOT_COUNT >= 2,
              "The FX3 save journal must retain more than one committed snapshot.");

struct QspiFlash {
    void* context;                         ///< Opaque context supplied to the callbacks.
    const uint8_t* bytes;                  ///< XIP/read-only view of the complete QSPI device.
    uint32_t size;                         ///< Size of the QSPI view in bytes.

    bool (*erase)(void* context, uint32_t offset, uint32_t size); ///< Erases aligned sectors.
    bool (*program)(void* context, uint32_t offset, const uint8_t* data,
                    uint32_t size);        ///< Programs aligned whole pages.
};

struct Record {
    bool valid;                            ///< True when a committed CRC-valid record exists.
    uint32_t sequence;                     ///< Monotonic sequence stored by the newest record.
    uint32_t slot;                         ///< Physical journal slot containing the record.
};

/// Returns the newest committed, CRC-valid record in the QSPI save journal.
Record scan(const QspiFlash& flash);

/// Restores the newest valid 128 KiB SRAM snapshot into destination.
bool restore(const QspiFlash& flash, uint8_t* destination, size_t destination_size,
             Record* restored_record = nullptr);

/// Appends one 128 KiB snapshot, reclaiming only the next slot when the ring is full.
bool append(const QspiFlash& flash, const uint8_t* snapshot, size_t snapshot_size,
            Record* written_record = nullptr);

/// Computes the CRC-32/ISO-HDLC value stored with each save snapshot.
uint32_t crc32(const uint8_t* data, size_t size);

} // namespace fx3_save
