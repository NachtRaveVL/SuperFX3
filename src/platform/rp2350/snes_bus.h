/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

#include "../../fx/fx_core.h"

// SNES cartridge-edge/control signals
struct SnesBusPins {
    uint8_t system_clk;     ///< SNES master system clock.
    uint8_t cpu_clock;      ///< SNES CPU clock.

    uint8_t rd_n;           ///< Active-low CPU read strobe.
    uint8_t wr_n;           ///< Active-low CPU write strobe.
    uint8_t romsel_n;       ///< Active-low cartridge ROM select.

    uint8_t reset_n;        ///< Active-low SNES reset signal.

    uint8_t refresh;        ///< SNES DRAM refresh signal.
    uint8_t wramsel_n;      ///< Active-low work RAM select.

    uint8_t irq_n;          ///< Active-low cartridge IRQ output.

    uint8_t pard_n;         ///< Active-low peripheral bus read strobe.
    uint8_t pawr_n;         ///< Active-low peripheral bus write strobe.

    uint8_t expand;         ///< SNES cartridge expansion signal.

    uint8_t data_dir;       ///< Controls external data-bus transceiver direction.
    uint8_t bus_oe_n;       ///< Active-low external bus transceiver output enable.
    uint8_t rom_oe_n;       ///< Active-low physical cartridge ROM output enable.
};

// Fixed RP2350B cartridge bus layout
static constexpr uint8_t SNES_ADDR_LO_BASE = 0;   // GPIO  0-15 = A0-A15
static constexpr uint8_t SNES_CTRL_BASE    = 16;  // GPIO 16-31 = controls
static constexpr uint8_t SNES_ADDR_HI_BASE = 32;  // GPIO 32-39 = A16-A23
static constexpr uint8_t SNES_DATA_BASE    = 40;  // GPIO 40-47 = D0-D7

static constexpr SnesBusPins SNES_BUS_PINS {
    16,     // SYSTEM CLK
    17,     // CPU_CLOCK
    18,     // /RD
    19,     // /WR
    20,     // /ROMSEL
    21,     // /RESET
    22,     // REFRESH
    23,     // /WRAMSEL
    24,     // /IRQ
    25,     // /PARD
    26,     // /PAWR
    27,     // EXPAND
    28,     // DATA_DIR
    29,     // /BUS_OE
    30      // /ROM_OE
};

static constexpr bool DATA_DIR_IN  = 0;
static constexpr bool DATA_DIR_OUT = 1;

static constexpr bool BUS_ENABLE  = 0;
static constexpr bool BUS_DISABLE = 1;

static constexpr bool ROM_ENABLE  = 0;
static constexpr bool ROM_DISABLE = 1;

static constexpr uint64_t SNES_ADDR_MASK = 0x000000000000FFFFull | (0xFFull << SNES_ADDR_HI_BASE);
static constexpr uint64_t SNES_DATA_MASK = 0xFFull << SNES_DATA_BASE;

// Cartridge bus

/// Configures the fixed RP2350B cartridge pins in a safe listening state.
void snes_bus_init(const SnesBusPins& pins);
/// Starts the PIO front end after the SuperFX core and backend are initialized.
void snes_bus_start(SuperFx& fx);
/// Services reset, ROM ownership, and core-1 requests for exclusive physical bus access.
void snes_bus_service();

/// Requests temporary physical-ROM bus ownership and reads one byte for legacy GSU1/2.
uint8_t snes_rom_read(void* context, uint32_t address);
/// Drives the active-low SNES cartridge IRQ input.
void snes_irq_write(void* context, bool asserted);
