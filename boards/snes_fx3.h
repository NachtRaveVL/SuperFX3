/*
 * Copyright (c) 2026 NR-RetroWorks
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

// NR-RetroWorks SNES FX3 cartridge board.
// RP2350B-based SNES cartridge with SuperFX / FX3 support.

#ifndef _BOARDS_SNES_FX3_H
#define _BOARDS_SNES_FX3_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define SNES_FX3

// --- RP2350 VARIANT ---

// RP2350B SC1510-A4, 80-pin package with GPIO0-GPIO47.
#define PICO_RP2350A 0

// --- SNES ADDRESS BUS ---

// GPIO0-GPIO15: A0-A15
#define SNES_ADDR_LO_BASE  0
#define SNES_ADDR_LO_COUNT 16

#define SNES_A0_PIN   0
#define SNES_A1_PIN   1
#define SNES_A2_PIN   2
#define SNES_A3_PIN   3
#define SNES_A4_PIN   4
#define SNES_A5_PIN   5
#define SNES_A6_PIN   6
#define SNES_A7_PIN   7
#define SNES_A8_PIN   8
#define SNES_A9_PIN   9
#define SNES_A10_PIN 10
#define SNES_A11_PIN 11
#define SNES_A12_PIN 12
#define SNES_A13_PIN 13
#define SNES_A14_PIN 14
#define SNES_A15_PIN 15

// GPIO32-GPIO39: A16-A23
#define SNES_ADDR_HI_BASE  32
#define SNES_ADDR_HI_COUNT 8

#define SNES_A16_PIN 32
#define SNES_A17_PIN 33
#define SNES_A18_PIN 34
#define SNES_A19_PIN 35
#define SNES_A20_PIN 36
#define SNES_A21_PIN 37
#define SNES_A22_PIN 38
#define SNES_A23_PIN 39

// --- SNES DATA BUS ---

// GPIO40-GPIO47: D0-D7
#define SNES_DATA_BASE  40
#define SNES_DATA_COUNT 8

#define SNES_D0_PIN 40
#define SNES_D1_PIN 41
#define SNES_D2_PIN 42
#define SNES_D3_PIN 43
#define SNES_D4_PIN 44
#define SNES_D5_PIN 45
#define SNES_D6_PIN 46
#define SNES_D7_PIN 47

// --- SNES CONTROL BUS ---

// GPIO16-GPIO18 are intentionally left unused. Keeping these upper-left
// package pins free shifts the cartridge control routing toward the bottom
// edge of the RP2350B and the control-bus level shifter.

// Optional 5A22 memory-cycle clock. Reserved for future timing work.
#define SNES_SYSCK_PIN     19

// Primary SNES cartridge control inputs.
#define SNES_RD_N_PIN      20
#define SNES_WR_N_PIN      21
#define SNES_CART_N_PIN    22
#define SNES_RESET_N_PIN   23

// Legacy GSU1/GSU2 signals. FX3 does not require these signals.
#define SNES_IRQ_N_PIN     24
#define SNES_PARD_N_PIN    25
#define SNES_PAWR_N_PIN    26

// /CART is the SNES cartridge ROM select signal.
#define SNES_ROMSEL_N_PIN SNES_CART_N_PIN

// --- BOARD BUS CONTROL ---

// One parallel ROM is the default. Populate ROM1 only when the bus image needs A23.
#ifndef SNES_PARALLEL_ROM_COUNT
#define SNES_PARALLEL_ROM_COUNT 1
#endif

#if SNES_PARALLEL_ROM_COUNT != 1 && SNES_PARALLEL_ROM_COUNT != 2
#error "SNES_PARALLEL_ROM_COUNT must be 1 or 2"
#endif

// GPIO27 is internal to the board firmware and is not connected to the
// SNES cartridge edge. PIO0 generates this signal from the address decode
// and PIO1/PIO2 use it to select SuperFX register transactions.
#define SNES_SERVICE_SEL_PIN 27

// GPIO28-GPIO31 are ordered for PCB routing. The ROM output-enables stay
// together, followed by the two control signals routed to the bus shifters.
#define SNES_ROM0_OE_N_PIN 28
#define SNES_ROM1_OE_N_PIN 29
#define SNES_BUS_OE_N_PIN  30
#define SNES_DATA_DIR_PIN  31

// --- BOARD SIGNAL POLARITY ---

#define SNES_DATA_DIR_IN  0
#define SNES_DATA_DIR_OUT 1

#define SNES_BUS_ENABLE  0
#define SNES_BUS_DISABLE 1

#define SNES_ROM_ENABLE  0
#define SNES_ROM_DISABLE 1

// --- GPIO GROUPS ---

#define SNES_CONTROL_BASE  16
#define SNES_CONTROL_COUNT 16

#define SNES_BUS_CTRL_BASE  28
#define SNES_BUS_CTRL_COUNT 4

// --- GPIO MASKS ---

#define SNES_ADDR_LO_MASK 0x000000000000FFFFULL
#define SNES_ADDR_HI_MASK 0x000000FF00000000ULL
#define SNES_ADDR_MASK    (SNES_ADDR_LO_MASK | SNES_ADDR_HI_MASK)
#define SNES_DATA_MASK    0x0000FF0000000000ULL

#define SNES_SYSCK_MASK      (1ULL << SNES_SYSCK_PIN)
#define SNES_RD_N_MASK       (1ULL << SNES_RD_N_PIN)
#define SNES_WR_N_MASK       (1ULL << SNES_WR_N_PIN)
#define SNES_CART_N_MASK     (1ULL << SNES_CART_N_PIN)
#define SNES_RESET_N_MASK    (1ULL << SNES_RESET_N_PIN)
#define SNES_IRQ_N_MASK      (1ULL << SNES_IRQ_N_PIN)
#define SNES_PARD_N_MASK     (1ULL << SNES_PARD_N_PIN)
#define SNES_PAWR_N_MASK     (1ULL << SNES_PAWR_N_PIN)
#define SNES_SERVICE_SEL_MASK (1ULL << SNES_SERVICE_SEL_PIN)
#define SNES_DATA_DIR_MASK    (1ULL << SNES_DATA_DIR_PIN)
#define SNES_BUS_OE_N_MASK    (1ULL << SNES_BUS_OE_N_PIN)
#define SNES_ROM0_OE_N_MASK   (1ULL << SNES_ROM0_OE_N_PIN)
#define SNES_ROM1_OE_N_MASK   (1ULL << SNES_ROM1_OE_N_PIN)
#define SNES_BUS_CTRL_MASK    (SNES_DATA_DIR_MASK | SNES_BUS_OE_N_MASK | SNES_ROM0_OE_N_MASK | SNES_ROM1_OE_N_MASK)

// --- PIO LAYOUT ---

// PIO0 operates on GPIO0-GPIO15 for the low address bus.
#define SNES_PIO_ADDR_LO_BASE SNES_ADDR_LO_BASE

// PIO1 uses GPIO16-GPIO31 for cartridge controls and the internal
// service-select signal.
#define SNES_PIO_CONTROL_BASE SNES_CONTROL_BASE

// GPIO32-GPIO47 are contiguous and contain A16-A23 followed by D0-D7.
#define SNES_PIO_ADDR_DATA_BASE 32
#define SNES_PIO_ADDR_DATA_COUNT 16

// Packed PIO capture layout:
//   bits  0-7  = A16-A23
//   bits  8-15 = D0-D7
//   bits 16-31 = A0-A15
#define SNES_CAPTURE_ADDR_HI_SHIFT 0
#define SNES_CAPTURE_DATA_SHIFT    8
#define SNES_CAPTURE_ADDR_LO_SHIFT 16

// PIO read-control mapping:
//   SET pin      = /ROM0_OE (GPIO28)
//   side-set 0   = /ROM1_OE (GPIO29)
//   side-set 1   = /BUS_OE  (GPIO30)
//   side-set 2   = DATA_DIR (GPIO31)
// This keeps the two ROM enables adjacent and the two shifter controls adjacent.
#define SNES_PIO_SET_BASE       SNES_ROM0_OE_N_PIN
#define SNES_PIO_SET_COUNT      1
#define SNES_PIO_SIDESET_BASE   SNES_ROM1_OE_N_PIN
#define SNES_PIO_SIDESET_COUNT  3

// --- FLASH ---

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (4 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
