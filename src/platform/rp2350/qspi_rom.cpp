/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "qspi_rom.h"

#include "snes_bus.h"
#include "storage/fx3_qspi_layout.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"

#include <cstring>

#if PICO_FLASH_ASSUME_CORE0_SAFE || PICO_FLASH_ASSUME_CORE1_SAFE
#error "QSPI installation requires SDK lockout on both cores"
#endif

namespace {
struct RomSector {
    uint32_t offset;
    const uint8_t* data;
    bool ok;
};

void program_sector(void* context) {
    auto& sector = *static_cast<RomSector*>(context);
    // The other core is parked and local interrupts are disabled. Only SDK
    // SRAM/ROM routines run without XIP; it is restored before verification.
    flash_range_erase(sector.offset, FLASH_SECTOR_SIZE);
    flash_range_program(sector.offset, sector.data, FLASH_SECTOR_SIZE);
    sector.ok = std::memcmp(reinterpret_cast<const void*>(XIP_BASE + sector.offset),
                            sector.data, FLASH_SECTOR_SIZE) == 0;
}
}

bool qspi_rom_program(void*, uint32_t offset, const uint8_t* data, uint32_t size) {
    if (!snes_bus_usb_mode() || !data || size != FLASH_SECTOR_SIZE ||
        offset % FLASH_SECTOR_SIZE != 0 || offset > fx3_qspi::FX_CODE_SIZE - size)
        return false;
    RomSector sector{fx3_qspi::FX_CODE_OFFSET + offset, data, false};
    return flash_safe_execute(program_sector, &sector, 1000) == PICO_OK && sector.ok;
}
