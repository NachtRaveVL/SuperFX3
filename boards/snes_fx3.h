/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

// NR-RetroWorks SNES FX3 production cartridge board.
// RP2350B-based SNES cartridge with SuperFX / FX3 support.

#ifndef _BOARDS_SNES_FX3_H
#define _BOARDS_SNES_FX3_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define SNES_FX3

// --- RP2350 VARIANT ---

// RP2350B SC1510-A4, 80-pin package with GPIO0-GPIO47.
#define PICO_RP2350A 0

// --- PRESENCE AND LOCAL BUS CONTROL ---

// Active-low console-present detector derived from cartridge-edge 5 V.
#define SNES_PRES_N_PIN       0

// GPIO1-GPIO7 are a contiguous RP2350-driven control group.
// /RD and /WR are the parallel-ROM strobes, not the translated SNES inputs.
#define SNES_ROM_RD_N_PIN     1
#define SNES_ROM_WR_N_PIN     2
#define SNES_ROM_CE_N_PIN     3
#define SNES_ADDR_OE_N_PIN    4
#define SNES_CONTROL_OE_N_PIN 5
#define SNES_DATA_DIR_PIN     6
#define SNES_DATA_OE_N_PIN    7

#define SNES_LOCAL_CONTROL_BASE  1
#define SNES_LOCAL_CONTROL_COUNT 7

// --- ROUTED ADDRESS BUS ---

// GPIO8-GPIO31 are physically contiguous but intentionally routed out of
// logical address order. Software must explicitly pack and unpack these bits.
#define SNES_A12_PIN  8
#define SNES_A11_PIN  9
#define SNES_A13_PIN 10
#define SNES_A10_PIN 11
#define SNES_A14_PIN 12
#define SNES_A9_PIN  13
#define SNES_A15_PIN 14
#define SNES_A8_PIN  15
#define SNES_A16_PIN 16
#define SNES_A7_PIN  17
#define SNES_A17_PIN 18
#define SNES_A6_PIN  19
#define SNES_A18_PIN 20
#define SNES_A5_PIN  21
#define SNES_A19_PIN 22
#define SNES_A4_PIN  23
#define SNES_A20_PIN 24
#define SNES_A3_PIN  25
#define SNES_A21_PIN 26
#define SNES_A2_PIN  27
#define SNES_A22_PIN 28
#define SNES_A1_PIN  29
#define SNES_A23_PIN 30
#define SNES_A0_PIN  31

#define SNES_ADDR_RAW_BASE  8
#define SNES_ADDR_RAW_COUNT 24

// --- TRANSLATED CONTROL BUS ---

// I_* signals are received from the console-side control translator when
// /C_OE is enabled. With /C_OE disabled, /I_RD and /I_WR remain input-only;
// firmware drives only /I_RST locally so that reset cannot float.
#define SNES_I_IRQ_N_PIN   32
#define SNES_I_CART_N_PIN  33
#define SNES_I_RD_N_PIN    34
#define SNES_I_WR_N_PIN    35
#define SNES_I_RESET_N_PIN 36
#define SNES_I_CLK_PIN     37

// O_* signals feed the open-drain 74LVC2G07 output path.
#define SNES_O_IRQ_N_PIN   38
#define SNES_O_RESET_N_PIN 39

// --- ROUTED DATA BUS ---

// GPIO40-GPIO47 are contiguous at PIO but routed D4,D0,D5,D1,D6,D2,D7,D3.
#define SNES_D4_PIN 40
#define SNES_D0_PIN 41
#define SNES_D5_PIN 42
#define SNES_D1_PIN 43
#define SNES_D6_PIN 44
#define SNES_D2_PIN 45
#define SNES_D7_PIN 46
#define SNES_D3_PIN 47

#define SNES_DATA_RAW_BASE  40
#define SNES_DATA_RAW_COUNT 8

// --- SIGNAL POLARITY AND DIRECTION ---

// Physical D_DIR truth table: 0 = B to A (SNES to RP2350), 1 = A to B.
#define SNES_DATA_DIR_IN  0
#define SNES_DATA_DIR_OUT 1

#define SNES_OUTPUT_ENABLE  0
#define SNES_OUTPUT_DISABLE 1

#define SNES_ROM_ENABLE  0
#define SNES_ROM_DISABLE 1

// Seven-bit words written to GPIO1-GPIO7 by the lower PIO control machine.
// Bit order: /RD, /WR, /ROM_CE, /A_OE, /C_OE, D_DIR, /D_OE.
#define SNES_CONTROL_CONSOLE_IDLE 0x07
#define SNES_CONTROL_DIRECT_READ  0x22
#define SNES_CONTROL_SERVICE_READ 0x27
#define SNES_CONTROL_BUS_ISOLATED 0x4F
#define SNES_CONTROL_ROM_READ     0x4A
#define SNES_CONTROL_ROM_VERIFY   0x5A
#define SNES_CONTROL_ROM_WRITE    0x59
#define SNES_CONTROL_ROM_WRITE_SETUP 0x5B
#define SNES_CONTROL_STANDALONE   0x5F

// --- GPIO MASKS ---

#define SNES_PRES_N_MASK        (1ULL << SNES_PRES_N_PIN)
#define SNES_LOCAL_CONTROL_MASK (0x7FULL << SNES_LOCAL_CONTROL_BASE)
#define SNES_ADDR_MASK          (0xFFFFFFULL << SNES_ADDR_RAW_BASE)
#define SNES_I_CONTROL_MASK     (0x3FULL << SNES_I_IRQ_N_PIN)
#define SNES_O_CONTROL_MASK     (0x3ULL << SNES_O_IRQ_N_PIN)
#define SNES_DATA_MASK          (0xFFULL << SNES_DATA_RAW_BASE)

// --- PACKED WRITE CAPTURE ---

// bits  0-7  = raw GPIO40-GPIO47 data order
// bits  8-31 = raw GPIO8-GPIO31 address order
#define SNES_CAPTURE_DATA_SHIFT     0
#define SNES_CAPTURE_ADDR_RAW_SHIFT 8

// --- PIO GPIO WINDOWS ---

#define SNES_PIO_LOWER_BASE 0
#define SNES_PIO_UPPER_BASE 16

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
