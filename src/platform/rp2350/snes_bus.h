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
/// True only while /SNES_PRES is deasserted and the translated console bus is isolated.
bool snes_bus_usb_mode();
/// True while translators isolate the console, including the boot-time ROM probe.
bool snes_bus_local_mode();

/// Requests temporary physical-ROM bus ownership and reads one byte for legacy GSU1/2.
uint8_t snes_rom_read(void* context, uint32_t address);
/// Drives active-low /O_IRQ for GSU completion.
void snes_irq_write(void* context, bool asserted);
/// Standalone USB parallel-ROM busy owner only; QSPI saves never call this.
void snes_busy_irq_write(void* context, bool asserted);
