/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <atomic>
#include <stdint.h>

#include "../../fx/fx_core.h"

struct Rp2350FxBackendContext {
    const uint8_t* rom;                                      ///< Linear FX ROM image stored in the RP2350 QSPI/XIP address space.
    uint32_t rom_size;                                       ///< Size of the linear FX ROM image in bytes.
    uint32_t ram_size;                                       ///< Size of CPU-visible FX SRAM in bytes.

    std::atomic<uint8_t>* ram;                               ///< Shared FX SRAM accessed by both RP2350 cores.

    uint8_t (*rom_read)(void* context, uint32_t offset);     ///< Reads one byte from a linear FX ROM offset.
    void (*irq_write)(void* context, bool asserted);         ///< Callback used to assert or release the SNES IRQ line.
};

/// Configures the backend to read the 3 MiB FX3 ROM from its reserved QSPI/XIP partition.
bool fx3_qspi_rom_init(Rp2350FxBackendContext& context);

/// Reads one byte from a linear offset in the 3 MiB FX3 QSPI ROM image.
uint8_t fx3_qspi_rom_read(void* context, uint32_t offset);

/// Builds the callback table used by the portable SuperFX core.
FxBackend fx_backend_create(Rp2350FxBackendContext* context);
