/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include "storage/parallel_rom_programmer.h"

/// Returns the byte-wide AMD/CFI parallel-ROM bus used only in local mode.
ParallelRomBus parallel_rom_gpio_bus();
