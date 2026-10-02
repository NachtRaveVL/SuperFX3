/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

void usb_rom_loader_init();
void usb_rom_loader_set_enabled(bool enabled);
void usb_rom_loader_task();
