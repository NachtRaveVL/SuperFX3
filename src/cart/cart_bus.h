/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stdint.h>

#include "../fx/fx_core.h"

// -----------------------------------------------------------------------------
// SNES cartridge-edge/control signals
// -----------------------------------------------------------------------------
struct CartControlPins {
    uint8_t system_clk;
    uint8_t cpu_clock;

    uint8_t rd_n;
    uint8_t wr_n;
    uint8_t cart_n;

    uint8_t reset_n;

    uint8_t refresh;
    uint8_t wramsel_n;

    uint8_t irq_n;

    uint8_t pard_n;
    uint8_t pawr_n;

    uint8_t expand;

    uint8_t data_dir;
    uint8_t bus_oe_n;
    uint8_t rom_oe_n;
};

// -----------------------------------------------------------------------------
// Fixed RP2350B cartridge bus layout
// -----------------------------------------------------------------------------
static constexpr uint8_t CART_ADDR_LO_BASE = 0;   // GPIO  0-15 = A0-A15
static constexpr uint8_t CART_CTRL_BASE    = 16;  // GPIO 16-31 = controls
static constexpr uint8_t CART_ADDR_HI_BASE = 32;  // GPIO 32-39 = A16-A23
static constexpr uint8_t CART_DATA_BASE    = 40;  // GPIO 40-47 = D0-D7

static constexpr CartControlPins CART_PINS {
    16,     // SYSTEM CLK
    17,     // CPU_CLOCK
    18,     // /RD
    19,     // /WR
    20,     // /CART
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

static constexpr bool BUS_ENABLE   = 0;
static constexpr bool BUS_DISABLE  = 1;

static constexpr bool ROM_ENABLE   = 0;
static constexpr bool ROM_DISABLE  = 1;

static constexpr uint64_t CART_ADDR_MASK =
    0x000000000000FFFFull |
    (0xFFull << CART_ADDR_HI_BASE);

static constexpr uint64_t CART_DATA_MASK =
    0xFFull << CART_DATA_BASE;

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------
void cart_bus_init(const CartControlPins& pins);

// -----------------------------------------------------------------------------
// GSU-side physical ROM access
// -----------------------------------------------------------------------------
uint8_t cart_rom_read(void* context, uint32_t address);

// -----------------------------------------------------------------------------
// /IRQ assertion
// -----------------------------------------------------------------------------
void cart_irq_write(void* context, bool asserted);

// -----------------------------------------------------------------------------
// SNES-side bus service
// -----------------------------------------------------------------------------
void cart_bus_service(SuperFx& fx);
