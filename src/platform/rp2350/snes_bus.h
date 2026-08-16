/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

#include "../../fx/fx_core.h"

// Cartridge bus

/// Configures the fixed RP2350B cartridge pins in a safe listening state.
void snes_bus_init();
/// Starts the PIO front end after the SuperFX core and backend are initialized.
void snes_bus_start(SuperFx& fx);
/// Services reset, ROM ownership, and core-1 requests for exclusive physical bus access.
void snes_bus_service();

/// Requests temporary physical-ROM bus ownership and reads one byte for legacy GSU1/2.
uint8_t snes_rom_read(void* context, uint32_t address);
/// Drives the active-low SNES cartridge IRQ input.
void snes_irq_write(void* context, bool asserted);
