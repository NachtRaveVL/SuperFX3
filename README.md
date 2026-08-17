# SuperFX3

SuperFX3 firmware for RP2350B-based SNES cartridges.

This project implements the Super FX / GSU processor family in firmware, with the current hardware target being SuperFX3. It combines a software GSU core with the RP2350B's dual cores, PIO hardware, DMA, internal SRAM, and QSPI flash to service the SNES cartridge bus while running the SuperFX processor alongside the console.

Copyright © 2026 NR-RetroWorks  
License: GNU GPL v3 or later

UNDER ACTIVE DEVELOPMENT -- WORK IN PROGRESS

The current host/static test suite passes. Hardware bring-up, logic analyzer testing, and final SNES timing validation are still in progress.

---

# Features

* Super FX / GSU instruction core with FX3 support
* RP2350B (SC1510-A4 80-QFN) running at 150 MHz
* Dual-core operation with SNES bus service on core 0 and FX code execution on core 1
* PIO/interrupt-based SNES cartridge bus decoding, read handling, write capture, and control
* 128 KiB (1 Mbit) of volatile shared cartridge RAM stored in RP2350 internal SRAM
  * 2 x 64 KiB banks at `$70:0000` and `$71:0000`
  * Leaves 392 KiB of RP2350 SRAM available for firmware and runtime use
  * 216×144 visible 8bpp SNES planar framebuffer in bank `$71`
* 4 MiB (32 Mbit) of flash storage on the RP2350 QSPI flash interface
  * Upper 3 MiB reserved for the FX3 ROM image
  * Lower 1 MiB available for RP2350 firmware
* Separate external parallel flash ROM for the SNES CPU, with one device by default and an optional second
  * Supports 1-64 Mbit devices, up to 128 Mbit total with two 64 Mbit ROMs
  * Supports LoROM, HiROM, ExLoROM, ExHiROM, extended SuperFX, and raw bus images
* FX3 8bpp PLOT/RPIX pixel-cache graphics path with native SNES planar output
* Planar-only framebuffer path with no separate chunky framebuffer allocation
  * FX3 MERGE C2P commands are skipped because PLOT writeback already produces final planar data
  * FX3 MERGE clear commands remain supported
* FX3 register access, RESET, STOP/GO, and cross-core synchronization
* Legacy GSU IRQ behavior retained for FX1/FX2 compatibility, while FX3 completion follows the hardware behavior of polling R15
* Host/static test suite with code coverage reporting
  * Host-side C++ tests for the processor core, opcodes, registers, synchronization, and backend
  * Static PIO tests that verify bus routing and board pin definitions
  * Coverage reporting for the portable processor core and host-testable RP2350 support code

The original GSU behavior is retained where useful for FX1/FX2 support, although the current cartridge and timing path are being developed specifically around FX3. Legacy cycle-accurate timing has not yet been validated on hardware.

---

# How It Works

The RP2350B is split into two main jobs.

* Core 0 services the SNES cartridge bus and handles communication between the console and the SuperFX core.
* Core 1 runs FX code while the GSU is in the running state.

PIO handles the timing-sensitive cartridge bus work. One PIO block decodes the SuperFX service area, another captures SNES writes, and another services reads and bus-control changes.

The SuperFX banks `$70-$71` are backed by 128 KiB of RP2350 internal SRAM. The planar framebuffer lives in bank `$71`, with no second chunky framebuffer allocated. The FX3 processor's private 3 MiB ROM image lives in a reserved partition at the top of the RP2350's primary QSPI flash.

The QSPI FX3 ROM is not the main SNES game ROM. The cartridge's normal parallel ROM remains available to the SNES 65816 side while the FX3 processor accesses its own ROM image. The parallel-ROM path is straight pass-through. The SNES address lines drive the ROM address pins directly, and an optional second device uses A23 to select the upper ROM. ROMs are striped into the physical device image before programming so CPU reads do not need a live address remap.

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
Internal SuperFX service select | GPIO27
`/ROM0_OE` | GPIO28
`/ROM1_OE` (optional) | GPIO29
`/BUS_OE` | GPIO30
DATA_DIR | GPIO31
A16-A23 | GPIO32-GPIO39
D0-D7 | GPIO40-GPIO47

GPIO27 is internal to the cartridge firmware and is not connected to the SNES cartridge edge. PIO0 generates this signal from the address decode and the other PIO bus handlers use it to identify SuperFX register transactions.

---

# Building

## Requirements

* Raspberry Pi Pico SDK 2.3.0 or newer
* ARM GCC toolchain with `arm-none-eabi-gcc` and `arm-none-eabi-g++`
* CMake
* Python 3 for the ROM image packing tools and diagnostic ROM build
* cc65 with `ca65` and `ld65` when building the FX3 diagnostic ROM

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

The main linked firmware output is:

```text
build/superfx3.elf
```

The ELF contains the linked firmware and symbols used for debugging. Because the project calls `pico_add_extra_outputs()`, the Pico SDK also generates the normal `.bin`, `.hex`, `.uf2`, map, and disassembly outputs. The raw `.bin` is used by the QSPI image packing tool below.

---

# FX3 Diagnostic ROM

The repository includes a hardware-facing SNES test application in `testrom/`. The 65816 owns the menu and validation while small GSU kernels exercise the FX3 implementation. This keeps the code doing the judging separate from the processor being tested.

Build the two diagnostic ROM halves with:

```bash
python3 testrom/build.py
```

Or through the configured CMake tree:

```bash
cmake --build build --target fx3_testrom
```

The build produces a matched diagnostic set. `fx3_test.sfc` is the 65816 supervisor for the parallel SNES ROM. `fx3_test_fxrom.bin` is the compact linked GSU payload. `fx3_test_fxrom_partition.bin` is the same payload expanded to the full 3 MiB private FX3 QSPI partition with erased space filled by `0xFF`. `fx3_test_manifest.json` records SHA-256 hashes, sizes, the QSPI offset, linked GSU entry points, and a pair ID tying the SNES and FX images together.

The first pass includes menu-driven and `RUN ALL` tests for register access, STOP, repeated START/STOP, shared RAM, ALU behavior, private ROM reads, an architectural ROM-to-ALU-to-RAM pipeline test, PLOT, a complete 8x8 PLOT tile, RPIX, all three CLEAR commands, and the current direct-to-planar C2P command behavior. Graphics tests are checked byte-for-byte in shared RAM before the result tile is copied to VRAM for visual inspection.

Run the source/layout checks without cc65 with:

```bash
python3 testrom/build.py --check
```

The diagnostic source/build-helper checks and the normal host/static firmware suite pass in the current tree. A native `ca65`/`ld65` assembly/link still needs to be run on a workstation with cc65 installed before treating the generated `.sfc` and QSPI payload as hardware-ready.

See `testrom/README.md` for the test ABI, programming-image options, validation status, and source layout.

---

# SNES Parallel ROM Image

The SNES CPU uses one parallel ROM by default with an optional second device for larger bus images. A 64 Mbit device uses A0-A22 directly, while smaller devices use the subset of address pins they implement. In dual-ROM builds, A23 selects ROM0 or ROM1. The source ROM is striped into the physical flash layout ahead of time instead of changing address lines during each CPU read. This keeps the normal ROM path entirely in PIO and external flash timing.

Create a programming image from a raw SNES ROM with:

```bash
python3 src/tools/make_snes_rom_image.py \
    path/to/game.sfc \
    build/game_rom0.bin \
    --map lorom \
    --chip-size-mbit 64 \
    --rom-count 1
```

Each populated ROM may be 1, 2, 4, 8, 16, 32, or 64 Mbit. One ROM is the default. A second ROM is only required when the striped image needs the A23=1 half of the bus or cannot fit the selected single-device capacity. The tool reports the minimum device size that can hold the selected mapping.

When ROM1 is populated, configure the firmware for two parallel ROMs:

```bash
cmake -S . -B build -DSNES_PARALLEL_ROM_COUNT=2
```

Supported map names are `lorom`, `hirom`, `exlorom`, `exhirom`, `superfx-extended`, and `raw`. LoROM and HiROM accept source images up to 4 MiB, ExLoROM and ExHiROM up to 8 MiB, and `superfx-extended` models the 11 MiB Snes9x extended SuperFX CPU map. `raw` accepts a prebuilt bus image up to the full 16 MiB / 128 Mbit physical ceiling. A 512-byte copier header is stripped automatically for mapped SNES ROMs.

The output file matches the selected physical ROM capacity and unused locations are filled with `0xFF`. With `--rom-count 2`, ROM1 is written separately using the `--rom1-output` path or an automatically generated `.rom1` filename. Banks `$70-$71` are still intercepted by the cartridge firmware for the shared 128 KiB SRAM.

---

# FX3 QSPI ROM Image

FX3 uses a private ROM image stored alongside the firmware in the RP2350's primary QSPI flash.

The current board definition uses 4 MiB of QSPI flash. The upper 3 MiB are reserved for the FX3 ROM, leaving the lower 1 MiB for firmware.

The firmware build produces `superfx3.elf` as the linked firmware binary and `superfx3.bin` as a flat flash image. The QSPI packing step uses the `.bin` because the combined image is written as raw flash data.

Create the combined QSPI image with:

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

Unused space and shorter FX3 ROM images are padded with `0xFF`.

---

# Testing

The portable core and the host-testable RP2350 support code have an extensive test suite.

From the `src` directory:

```bash
bash tests/run_tests.sh
```

Calling the script through `bash` avoids executable-bit issues when the tree is unpacked on Windows/WSL.

This runs the C++ processor tests, opcode tests, synchronization tests, register/backend tests, ROM image packer tests, PIO static tests, and a strict production-source compile/link check using Pico SDK stubs.

For coverage:

```bash
bash tests/run_coverage.sh
```

The coverage report is written to:

```text
build/coverage/coverage_report.txt
```

Current coverage gates are:

* Portable `fx/*.cpp` line coverage of at least 95%
* Host-testable core, sync, and backend line coverage of at least 90%
* Branch alternatives taken at least once of at least 75%

The current host/static test suite and coverage thresholds pass. These tests are useful for catching processor and integration mistakes, but they are not a replacement for testing the actual SNES bus. PIO timing, GPIO behavior, DMA, interrupt latency, and cartridge electrical timing still need to be verified on real hardware.

---

# Source Layout

Path | Description
--- | ---
`boards/snes_fx3.h` | RP2350B board definition and complete cartridge GPIO map
`src/fx/` | SuperFX processor core, opcodes, registers, memory, and graphics
`src/platform/rp2350/` | RP2350 backend, cross-core synchronization, SNES bus, PIO, and DMA support
`src/tests/` | Host C++ tests, PIO static tests, SDK stubs, and coverage tools
`src/tools/` | SNES parallel-ROM and FX3 QSPI image utilities
`src/main.cpp` | Firmware startup, shared RAM, multicore setup, and main service loop

---

# Current Status

The host/static suite is passing and the firmware build is set up for the Pico SDK and ARM toolchain. Hardware validation is still the next major step.

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

Dedicated to Rebecca Ann Heineman and Jennell Allyn Jaquays.

---

# License

SuperFX3 is licensed under the GNU General Public License, version 3 or later.

See `LICENSE` for the complete license text.