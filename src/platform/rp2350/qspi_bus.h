/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

// All flash writers and SD transactions share this nonblocking owner.
bool qspi_bus_try_acquire();
void qspi_bus_release();

// Core 1 requests live SRAM bus service; Core 0 acknowledges from that service.
bool qspi_bus_audio_begin();
void qspi_bus_audio_end();
void qspi_bus_core0_service();
void qspi_bus_audio_cancel();
bool qspi_bus_audio_cancelled();
