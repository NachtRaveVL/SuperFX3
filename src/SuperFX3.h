/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 *
 * Arduino library compatibility header.
 *
 * SuperFX3 firmware is built with the Raspberry Pi Pico SDK. This umbrella
 * header exposes the platform-independent Super FX / GSU core API while also
 * providing the conventional src/SuperFX3.h entry point expected by Arduino
 * library tooling.
 */

#pragma once

#include "fx/fx_core.h"
#include "platform/rp2350/snes_bus.h"
#include "platform/rp2350/fx_backend.h"
#include "platform/rp2350/fx_sync.h"
