/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

#include "../../fx/fx_core.h"

/// Initializes cross-core SuperFX ownership, command queues, and runtime snapshots.
void fx_sync_init(SuperFx& fx, const FxBackend& backend);

/// Services queued CPU writes and executes one SuperFX instruction on core 1.
bool fx_sync_core1_service();

/// Provides a core-0-safe SNES register read using snapshots while core 1 owns state.
uint8_t fx_sync_cpu_read(uint16_t addr);
/// Applies or queues a SNES register write according to current core ownership.
/// @return True when the write was applied or queued, or false when the command queue is full.
bool fx_sync_cpu_write(uint16_t addr, uint8_t value);

/// Provides a core-0-safe SNES RAM read while honoring the published ownership state.
uint8_t fx_sync_cpu_ram_read(uint32_t addr);
/// Provides a core-0-safe SNES RAM write while honoring the published ownership state.
void fx_sync_cpu_ram_write(uint32_t addr, uint8_t value);

/// Returns the latest synchronized SNES ROM ownership state.
bool fx_sync_rom_access_allowed();
/// Returns the latest synchronized SNES RAM ownership state.
bool fx_sync_ram_access_allowed();
/// Returns the blocked-ROM data pattern without taking the FxState lock.
uint8_t fx_sync_blocked_rom_value(uint32_t addr);

/// Resets immediately or queues reset for core 1 when it owns the SuperFX state.
/// @return True when reset was applied or successfully queued, or false when the command queue is full.
bool fx_sync_reset();
