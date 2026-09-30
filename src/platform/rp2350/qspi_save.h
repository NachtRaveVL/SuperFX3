/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <atomic>
#include <stdint.h>

/// Restores persistent SRAM before either the SNES or core 1 can access it.
void qspi_save_init(std::atomic<uint8_t>* ram);
/// Saves on console reset or loss of SNES presence, while power remains available.
/// Never pauses a live game solely because SRAM changed.
void qspi_save_task();
bool qspi_save_last_ok();
