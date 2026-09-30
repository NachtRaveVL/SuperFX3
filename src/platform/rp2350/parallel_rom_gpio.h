/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "storage/parallel_rom_programmer.h"

/// Returns the byte-wide IS29GL128 bus implementation used only in USB mode.
ParallelRomBus parallel_rom_gpio_bus();
