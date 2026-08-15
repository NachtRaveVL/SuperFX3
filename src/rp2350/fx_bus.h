/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

#include "../fx/fx_core.h"

struct Rp2350FxBusContext {
    uint32_t rom_size;
    uint32_t ram_size;
    uint8_t* ram;
};

FxBus fx_bus_create(Rp2350FxBusContext* context);
