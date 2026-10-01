/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

namespace fx3_qspi {

// Fixed W25Q32 production layout. This QSPI device never contains the SNES
// game/program ROM; that image belongs exclusively to the external parallel NOR.
constexpr uint32_t FLASH_SIZE = 4u * 1024u * 1024u;
constexpr uint32_t FIRMWARE_OFFSET = 0u;
constexpr uint32_t FIRMWARE_SIZE = 496u * 1024u;
constexpr uint32_t SAVE_OFFSET = FIRMWARE_OFFSET + FIRMWARE_SIZE;
constexpr uint32_t SAVE_SIZE = 528u * 1024u;
constexpr uint32_t FX_CODE_OFFSET = SAVE_OFFSET + SAVE_SIZE;
constexpr uint32_t FX_CODE_SIZE = 3u * 1024u * 1024u;

constexpr uint32_t FLASH_PAGE_SIZE = 256u;
constexpr uint32_t FLASH_SECTOR_SIZE = 4096u;

static_assert((SAVE_OFFSET & (FLASH_SECTOR_SIZE - 1u)) == 0,
              "The FX3 save partition must start on a flash-sector boundary.");
static_assert((SAVE_SIZE & (FLASH_SECTOR_SIZE - 1u)) == 0,
              "The FX3 save partition size must be flash-sector aligned.");
static_assert((FX_CODE_OFFSET & (FLASH_SECTOR_SIZE - 1u)) == 0,
              "The FX code partition must start on a flash-sector boundary.");
static_assert(FIRMWARE_OFFSET + FIRMWARE_SIZE == SAVE_OFFSET,
              "The firmware and save partitions must be contiguous.");
static_assert(SAVE_OFFSET + SAVE_SIZE == FX_CODE_OFFSET,
              "The save and FX code partitions must be contiguous.");
static_assert(FX_CODE_OFFSET == 0x100000u,
              "The production FX code partition must remain at 0x100000.");
static_assert(FX_CODE_OFFSET + FX_CODE_SIZE == FLASH_SIZE,
              "The three QSPI partitions must cover the complete W25Q32.");

} // namespace fx3_qspi
