# SuperFX3

SuperFX3 firmware for RP2350B based SNES cartridges.

This project implements the Super FX / GSU processor family in firmware, with the current hardware target being SuperFX3. It combines a software GSU core with the RP2350B's dual cores, PIO hardware, DMA, internal SRAM, and QSPI flash to service the SNES cartridge bus while running the SuperFX processor alongside the console.

Copyright © 2026 NR-RetroWorks  
License: GNU GPL v3 or later

UNDER ACTIVE DEVELOPMENT -- WORK IN PROGRESS

The firmware is now building cleanly with the real RP2350 toolchain and the complete host/static test suite is passing. Hardware bring-up, logic analyzer testing, and final SNES timing validation are still in progress.

---

# Features

* Super FX / GSU instruction core with FX3 support
* RP2350B running at 150 MHz
* Dual-core operation with SNES bus service on core 0 and SuperFX execution on core 1
* PIO-based SNES cartridge bus decoding, read handling, write capture, and control
* 128 KiB of shared SuperFX RAM stored in RP2350 internal SRAM
* 3 MiB private FX3 ROM stored in the RP2350's primary QSPI flash
* Separate external parallel cartridge ROM for the SNES 65816 side
* FX3 8bpp PLOT and pixel-cache graphics path
* FX3 direct-to-planar draw, read, and clear commands (no chunky framebuffer conversions)
* SuperFX register access, IRQ, RESET, STOP/GO, and cross-core synchronization
* Host-side C++ tests for the processor core, opcodes, registers, synchronization, and backend
* Static PIO tests that verify the bus routing and board pin definitions
* Coverage reporting for the portable processor core and host-testable RP2350 support code

The original GSU behavior is retained where useful for FX1/FX2 support, although the current cartridge and timing path are being developed specifically around FX3. Legacy cycle-accurate timing has not yet been validated on hardware.

---

# How It Works

The RP2350B is split into two main jobs.

* Core 0 services the SNES cartridge bus and handles communication between the console and the SuperFX core.
* Core 1 runs the SuperFX processor whenever the GSU has ownership and is in the running state.

PIO handles the timing-sensitive cartridge bus work. One PIO block decodes the SuperFX service area, another captures SNES writes, and another services reads and bus-control changes.

The SuperFX banks `$70-$71` are backed by 128 KiB of RP2350 internal SRAM. The FX3 processor's private 3 MiB ROM image lives in a reserved partition at the top of the RP2350's primary QSPI flash.

The QSPI FX3 ROM is not the main SNES game ROM. The cartridge's normal parallel ROM remains available to the SNES 65816 side while the FX3 processor accesses its own ROM image.

---

# Hardware

This firmware is written specifically for the NR-RetroWorks RP2350B SNES FX3 cartridge board.

The hardware definition lives in:

```text
boards/snes_fx3.h
```

CMake selects `snes_fx3` automatically and will reject a different Pico board definition.

## RP2350B Pin Setup

Signal | RP2350B GPIO
--- | ---
A0-A15 | GPIO0-GPIO15
SYSTEM CLK | GPIO16
CPU CLOCK | GPIO17
`/RD` | GPIO18
`/WR` | GPIO19
`/CART` / `/ROMSEL` | GPIO20
`/RESET` | GPIO21
REFRESH | GPIO22
`/WRAMSEL` | GPIO23
`/IRQ` | GPIO24
`/PARD` | GPIO25
`/PAWR` | GPIO26
EXPAND | GPIO27
DATA_DIR | GPIO28
`/BUS_OE` | GPIO29
`/ROM_OE` | GPIO30
Internal SuperFX service select | GPIO31
A16-A23 | GPIO32-GPIO39
D0-D7 | GPIO40-GPIO47

GPIO31 is internal to the cartridge firmware and is not connected to the SNES cartridge edge. PIO0 generates this signal from the address decode and the other PIO bus handlers use it to identify SuperFX register transactions.

---

# Building

## Requirements

* Raspberry Pi Pico SDK 2.3.0 or newer
* ARM GCC toolchain with `arm-none-eabi-gcc` and `arm-none-eabi-g++`
* CMake
* Python 3 for the QSPI image packing tool

Set `PICO_SDK_PATH` to your Pico SDK installation if it is not already configured:

```bash
export PICO_SDK_PATH="$HOME/pico/pico-sdk"
```

Configure and build from the repository root:

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

The project uses C++17 and automatically selects the custom `snes_fx3` RP2350B board definition.

Normal Pico SDK output files are generated under `build/`, including `superfx3.elf` and the additional flashable image formats.

---

# FX3 QSPI ROM Image

FX3 uses a private ROM image stored alongside the firmware in the RP2350's primary QSPI flash.

The current board definition uses 4 MiB of QSPI flash. The upper 3 MiB are reserved for the FX3 ROM, leaving the lower 1 MiB for firmware.

A helper tool is included to combine the firmware and FX3 ROM into one raw flash image:

```bash
python3 src/tools/make_fx3_qspi_image.py \
    build/superfx3.bin \
    path/to/fx3_rom.bin \
    build/superfx3_qspi.bin
```

The tool checks that:

* The flash is at least 4 MiB
* The FX3 ROM is no larger than 3 MiB
* The ROM partition is aligned to a 4 KiB flash sector
* The firmware does not overlap the FX3 ROM partition

Shorter FX3 ROM images are padded with `0xFF`.

---

# Testing

The portable core and the host-testable RP2350 support code have a fairly extensive test suite.

From the `src` directory:

```bash
./tests/run_tests.sh
```

This runs the C++ processor tests, opcode tests, synchronization tests, register/backend tests, graphics packer tests, PIO static tests, and a strict production-source compile/link check using Pico SDK stubs.

For coverage:

```bash
./tests/run_coverage.sh
```

The coverage report is written to:

```text
build/coverage/coverage_report.txt
```

Current coverage gates are:

* Portable `fx/*.cpp` line coverage of at least 95%
* Host-testable core, sync, and backend line coverage of at least 90%
* Branch alternatives taken at least once of at least 75%

The host tests are useful for catching processor and integration mistakes, but they are not a replacement for testing the actual SNES bus. PIO timing, GPIO behavior, DMA, interrupt latency, and cartridge electrical timing still need to be verified on real hardware.

---

# Source Layout

Path | Description
--- | ---
`boards/snes_fx3.h` | RP2350B board definition and complete cartridge GPIO map
`src/fx/` | SuperFX processor core, opcodes, registers, memory, and graphics
`src/platform/rp2350/` | RP2350 backend, cross-core synchronization, SNES bus, PIO, and DMA support
`src/tests/` | Host C++ tests, PIO static tests, SDK stubs, and coverage tools
`src/tools/` | Firmware and FX3 QSPI image utilities
`src/main.cpp` | Firmware startup, shared RAM, multicore setup, and main service loop

---

# Current Status

The software side is far enough along to build cleanly against the actual Pico SDK and ARM toolchain, and the host/static test suite currently passes.

The next major step is hardware validation.

Things still being worked through include:

* Real SNES bus timing and logic analyzer verification
* PIO transaction timing under actual cartridge load
* Cross-core queue depth and worst-case service latency
* Final FX3 behavior checks against hardware and trusted traces
* Legacy GSU1/GSU2 timing if those modes are kept as supported targets

Expect things to move around while hardware bring-up continues.

---

# Credits

Portions of the SuperFX processor implementation are based on the GSU implementation from [Mesen Community Edition](https://github.com/nesdev-org/MesenCE), which is licensed under the GNU GPL.

Special thanks to Randy Linden and kandowantu.

Dedicated to Rebecca Heinemann and Jennel Jacquays.

---

# License

SuperFX3 is licensed under the GNU General Public License, version 3 or later.

See `LICENSE` for the complete license text.
