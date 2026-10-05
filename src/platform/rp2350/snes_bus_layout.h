/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

#include "pico.h"

/// Converts the raw GPIO8-GPIO31 routed order into logical SNES A0-A23 order.
static __force_inline uint32_t snes_unpack_address_raw(uint32_t raw) {
    raw &= 0x00FFFFFFu;
    return ((raw >> 23) & 1u) << 0 |
           ((raw >> 21) & 1u) << 1 |
           ((raw >> 19) & 1u) << 2 |
           ((raw >> 17) & 1u) << 3 |
           ((raw >> 15) & 1u) << 4 |
           ((raw >> 13) & 1u) << 5 |
           ((raw >> 11) & 1u) << 6 |
           ((raw >> 9) & 1u) << 7 |
           ((raw >> 7) & 1u) << 8 |
           ((raw >> 5) & 1u) << 9 |
           ((raw >> 3) & 1u) << 10 |
           ((raw >> 1) & 1u) << 11 |
           ((raw >> 0) & 1u) << 12 |
           ((raw >> 2) & 1u) << 13 |
           ((raw >> 4) & 1u) << 14 |
           ((raw >> 6) & 1u) << 15 |
           ((raw >> 8) & 1u) << 16 |
           ((raw >> 10) & 1u) << 17 |
           ((raw >> 12) & 1u) << 18 |
           ((raw >> 14) & 1u) << 19 |
           ((raw >> 16) & 1u) << 20 |
           ((raw >> 18) & 1u) << 21 |
           ((raw >> 20) & 1u) << 22 |
           ((raw >> 22) & 1u) << 23;
}

/// Converts logical SNES A0-A23 order into the raw GPIO8-GPIO31 routed order.
static __force_inline uint32_t snes_pack_address_raw(uint32_t address) {
    address &= 0x00FFFFFFu;
    return ((address >> 12) & 1u) << 0 |
           ((address >> 11) & 1u) << 1 |
           ((address >> 13) & 1u) << 2 |
           ((address >> 10) & 1u) << 3 |
           ((address >> 14) & 1u) << 4 |
           ((address >> 9) & 1u) << 5 |
           ((address >> 15) & 1u) << 6 |
           ((address >> 8) & 1u) << 7 |
           ((address >> 16) & 1u) << 8 |
           ((address >> 7) & 1u) << 9 |
           ((address >> 17) & 1u) << 10 |
           ((address >> 6) & 1u) << 11 |
           ((address >> 18) & 1u) << 12 |
           ((address >> 5) & 1u) << 13 |
           ((address >> 19) & 1u) << 14 |
           ((address >> 4) & 1u) << 15 |
           ((address >> 20) & 1u) << 16 |
           ((address >> 3) & 1u) << 17 |
           ((address >> 21) & 1u) << 18 |
           ((address >> 2) & 1u) << 19 |
           ((address >> 22) & 1u) << 20 |
           ((address >> 1) & 1u) << 21 |
           ((address >> 23) & 1u) << 22 |
           ((address >> 0) & 1u) << 23;
}

/// Converts raw GPIO40-GPIO47 order into logical SNES D0-D7 order.
static __force_inline uint8_t snes_unpack_data_raw(uint8_t raw) {
    return static_cast<uint8_t>(
        ((raw >> 1) & 1u) << 0 |
        ((raw >> 3) & 1u) << 1 |
        ((raw >> 5) & 1u) << 2 |
        ((raw >> 7) & 1u) << 3 |
        ((raw >> 0) & 1u) << 4 |
        ((raw >> 2) & 1u) << 5 |
        ((raw >> 4) & 1u) << 6 |
        ((raw >> 6) & 1u) << 7
    );
}

/// Converts logical SNES D0-D7 order into raw GPIO40-GPIO47 order.
static __force_inline uint8_t snes_pack_data_raw(uint8_t data) {
    return static_cast<uint8_t>(
        ((data >> 4) & 1u) << 0 |
        ((data >> 0) & 1u) << 1 |
        ((data >> 5) & 1u) << 2 |
        ((data >> 1) & 1u) << 3 |
        ((data >> 6) & 1u) << 4 |
        ((data >> 2) & 1u) << 5 |
        ((data >> 7) & 1u) << 6 |
        ((data >> 3) & 1u) << 7
    );
}

static __force_inline uint32_t snes_address_from_gpio(uint64_t gpio) {
    return snes_unpack_address_raw(
        static_cast<uint32_t>((gpio & SNES_ADDR_MASK) >> SNES_ADDR_RAW_BASE)
    );
}

static __force_inline uint8_t snes_data_from_gpio(uint64_t gpio) {
    return snes_unpack_data_raw(
        static_cast<uint8_t>((gpio & SNES_DATA_MASK) >> SNES_DATA_RAW_BASE)
    );
}

static __force_inline uint64_t snes_address_to_gpio(uint32_t address) {
    return static_cast<uint64_t>(snes_pack_address_raw(address)) << SNES_ADDR_RAW_BASE;
}

static __force_inline uint64_t snes_data_to_gpio(uint8_t data) {
    return static_cast<uint64_t>(snes_pack_data_raw(data)) << SNES_DATA_RAW_BASE;
}
