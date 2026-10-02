/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <atomic>
#include <stdint.h>

/// Restores persistent SRAM before either the SNES or core 1 can access it.
void qspi_save_init(std::atomic<uint8_t>* ram);
/// Synchronous SAVE_AND_STOP backend, called by core 1 after guest execution stops.
/// Parks the other core through snapshot/commit/verify; never signals an IRQ.
bool qspi_save_now(void* context);
/// Saves on console reset or loss of SNES presence, while power remains available.
/// Never pauses a live game solely because SRAM changed.
void qspi_save_task();
bool qspi_save_last_ok();
