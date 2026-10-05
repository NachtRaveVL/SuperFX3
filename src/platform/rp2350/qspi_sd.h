/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

enum class QspiSdResult : uint8_t { Ok, Unavailable, Busy, Cancelled, Timeout, ProtocolError };

// Core 1 only. One complete SPI transaction, with both buffers in SRAM.
// Null TX clocks $FF; null RX discards input. CS is never held across calls.
// select=false supplies startup clocks with SD deselected. clock_hz is a ceiling.
QspiSdResult qspi_sd_exchange(const uint8_t* tx, uint8_t* rx, uint32_t size,
                             uint32_t clock_hz = 4000000u, bool select = true);

// Six-byte SD command, R1/R3/R7 response, and optional data block under one CS.
// block must have room for block_size payload bytes plus the two CRC bytes.
QspiSdResult qspi_sd_command(const uint8_t* command, uint8_t* response,
                            uint32_t response_size, uint8_t* block = nullptr,
                            uint32_t block_size = 0, uint32_t clock_hz = 4000000u);
