# FX3 Diagnostic ROM

This is the hardware-facing test application for the SuperFX3 cartridge.

The SNES 65816 is the supervisor. It owns the menu, controller input, PPU setup, timeouts, validation, and result display. The GSU is treated as the device under test. A broken GSU should not be able to take the diagnostic UI down with it.

The source layout follows the useful part of the old Star Fox style of keeping SNES CPU code and GSU code separate. The build does not use the original ARGSFX or DOSBox toolchain. Both sides are assembled with native cc65 tools.

## Outputs

A normal build creates a matched four-file set:

```text
fx3_test.sfc
fx3_test_fxrom.bin
fx3_test_fxrom_partition.bin
fx3_test_manifest.json
```

`fx3_test.sfc` is the LoROM image for the normal parallel SNES ROM.

`fx3_test_fxrom.bin` is the compact 32 KiB linked GSU payload. It is useful when a programmer or packing tool wants only the bytes that contain test code.

`fx3_test_fxrom_partition.bin` is the programming-ready private FX3 ROM partition. It is exactly 3 MiB, starts with the linked payload, and fills the unused space with `0xFF`. With the current 4 MiB QSPI layout this partition belongs at flash offset `0x100000`. The build imports those layout values from `make_fx3_qspi_image.py` so the diagnostic tooling does not maintain a second copy of them.

`fx3_test_manifest.json` records the SHA-256 and size of every artifact, the QSPI partition offset, each linked GSU entry point, and a pair ID calculated from the SNES supervisor plus the programming-ready FX partition. That gives us a simple way to catch a mismatched `.sfc` and GSU image at the bench.

The build links the GSU image first and generates `fx_entries.inc` from its symbol file. The 65816 supervisor then consumes those generated entry points. GSU routines can move around without hand-maintained addresses in the SNES program. The binaries stay separate, so GSU work can be reflashed in the private QSPI partition without changing the parallel ROM when the supervisor itself has not changed.

## Toolchain

Install cc65 so `ca65` and `ld65` are on `PATH`. `CC65_HOME` is also supported.

Then build from the repository root:

```bash
python3 testrom/build.py
```

The same build is available through CMake:

```bash
cmake --build build --target fx3_testrom
```

A source-only check does not require cc65:

```bash
python3 testrom/build.py --check
```

No DOS environment is required.

## Validation Status

The source generators, registry checks, firmware ABI drift checks, QSPI staging helpers, and manifest helpers are exercised by `python3 testrom/build.py --check`. That check is also part of the main host test suite.

The complete host/static firmware suite passes with this first-pass diagnostic framework in the tree, including the architectural core tests, stateful SNES bus simulation, single- and dual-ROM configurations, synchronization tests, and strict production stub links.

## Programming Images

To also stripe the `.sfc` for the physical parallel ROM:

```bash
python3 testrom/build.py --pack-parallel --chip-size-mbit 64 --rom-count 1
```

To build a complete QSPI image when `superfx3.bin` already exists:

```bash
python3 testrom/build.py --firmware-bin build/superfx3.bin
```

That uses the same QSPI image packer as the rest of the project.

## Test Architecture

`tests.json` is the test registry. Each entry names a GSU kernel, setup operation, validator, timeout, and optional parameters. The menu and `RUN ALL` path use the same registry.

Current test families include:

* FX3 presence and CPU-visible register access
* GSU STOP and repeated START/STOP
* Shared RAM writes
* A basic ALU result written back through shared RAM
* Private FX ROM buffering through R14 and GETB
* An architectural private-ROM to ALU to delayed-RAM-store to STOP chain
* 8bpp PLOT with byte-for-byte planar validation
* A complete 8x8 PLOT tile that exercises repeated pixel-cache handoffs
* RPIX round trip
* FX3 CLEAR A, B, and C with full tile-pattern checks and neighbor guards
* FX3 C2P A, B, and C no-op checks for the current direct-to-planar firmware architecture

Visual tests also copy an actual 8bpp tile from bank `$71` into SNES VRAM. The BG1 map is blank everywhere except for one visual-result tile, so test data cannot repeat across the whole screen. TIMEOUT results keep BG1 hidden because the output buffer may still contain setup sentinels rather than a meaningful result. The screen is therefore useful to a human, but the screen is not the oracle. The 65816 checks the shared RAM bytes first.

## Source Tree

```text
cpu/       65816 supervisor, UI, PPU code, input, runner, validators
fx/        small independent GSU test kernels
include/   SNES, FX3, and test ABI constants
linker/    separate linker layouts for the parallel ROM and private FX ROM
tools/     registry, font, symbol, and build helpers
generated/ generated font, registry, and linked GSU entry points
tests.json table-driven test definitions
```

The local `fx/gsu.inc` only defines the small instruction subset the current tests need. Its pseudo-ops follow the same useful `WITH` conventions used by ARM9's `casfx` macros, while keeping this project on the normal ca65 build path. The `casfx` source is not vendored or copied into this project. More helpers can be added as the suite grows.

## Reference Projects

UltraStarFox and UltraStarFox2 are useful references for how real Star Fox codebases keep 65816 code, MARIO code, includes, fonts, data, and tools separated. We are borrowing that organizational lesson without carrying their ARGSFX and DOSBox build requirements into this project.

* https://github.com/Sunlitspace542/ultrastarfox
* https://github.com/Sunlitspace542/ultrastarfox2

Rebecca Heineman's Burgerlib is MIT licensed and its history goes back to 65816 code on the SNES and Apple IIgs. It contains highly optimized 65816 code amongst other useful functionality.

* https://github.com/Olde-Skuul/burgerlib

ARM9's `casfx` is the other useful reference for readable SuperFX source on ca65. The local macro layer is intentionally much smaller, but its instruction encodings and same-register `WITH` optimizations were cross-checked against it.

* https://github.com/ARM9/casfx
