/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#pragma once

#include "../../fx/fx_core.h"

/// Starts the PIO state machines that monitor SNES bus transactions.
void snes_pio_start(SuperFx& fx);
/// Temporarily disconnects PIO control of the cartridge bus.
void snes_pio_pause();
/// Restores PIO control after a GSU-side physical ROM access.
void snes_pio_resume();
/// Records a new GSU ROM ownership state and applies it immediately when /RD is idle.
void snes_pio_request_rom_ownership(bool blocked);
/// Applies any deferred GSU ROM ownership change at a safe /RD-high point.
void snes_pio_sync_rom_ownership();
/// Returns true when the PIO reset watcher has observed /RESET asserted.
bool snes_pio_reset_pending();
/// Clears the reset request after the SuperFX core has been reset.
void snes_pio_clear_reset();
