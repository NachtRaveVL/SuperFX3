/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

/// Programs one SRAM-backed sector of the FX partition during USB installation.
bool qspi_rom_program(void* context, uint32_t offset, const uint8_t* data, uint32_t size);
