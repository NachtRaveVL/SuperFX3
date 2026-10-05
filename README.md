# SuperFX3 FOSS Firmware

SuperFX3 FOSS firmware for RP2350B-based SNES cartridges utilizing NR-RetroWorks developed interfacing.

**SuperFX3 Firmware v0.10.2**

This project implements the Super FX / GSU processor family in firmware, with SuperFX3 as the current hardware target. The RP2350B handles the SNES cartridge bus, runs the GSU core, and provides shared RAM plus FX-visible ROM storage.

Copyright © 2026 NR-RetroWorks  
License: GNU GPL v3 or later

**UNDER ACTIVE DEVELOPMENT - WORK IN PROGRESS**

Hardware is currently in prototype phase and work continues on hardware validation, testing, and GSU-1/2 timing.

---

# Features

* Super FX / GSU instruction core with FX3 support
* RP2350B (SC1510-A4 80-QFN) running at 150 MHz
* Dual-core operation
  * Core 0 services the SNES cartridge bus
  * Core 1 executes FX code
* PIO/interrupt-based SNES bus decoding, read handling, write capture, and control
* 128 KiB (1 Mbit) of volatile shared cartridge RAM in RP2350 SRAM
  * 2 x 64 KiB banks at `$70:0000` and `$71:0000`
  * Remaining on-chip SRAM is used by firmware, save staging, USB/NOR buffers, and runtime state
  * Default 216x144 visible 8bpp planar framebuffer in bank `$71`
* 4 MiB (32 Mbit) RP2350 QSPI flash
  * Lower 496 KiB for firmware
  * 528 KiB append-only SRAM save journal
    * 4 rotating saves for wear leveling
  * Upper 3 MiB for FX-visible game ROM (lower 3 MiB ROM copy)
* Separate parallel flash ROM for the SNES CPU
  * 8-128 Mbit TSOP48/56 device capacity with full A0-A23 routing
  * LoROM, HiROM, ExLoROM, ExHiROM, FX3, and raw bus images
* SD card A/V streaming
  * Full-screen video with synchronized SPC700 audio
  * Standalone voice-over and dialogue playback
* FX3 2/4/8bpp PLOT/RPIX pixel-cache graphics path with programmable SCBR and native SNES planar output
* Planar-only framebuffer path with no separate chunky framebuffer
  * FX3 MERGE C2P commands are skipped because PLOT writeback already produces planar data
  * FX3 MERGE clear commands remain supported
  * `ALT1; MERGE` retains the original GSU MERGE operation
* `ALT3; STOP` is terminal SAVE_AND_STOP (invocated save state), with completion only after durable QSPI save
* FX3 register access, RESET, STOP/GO, and cross-core synchronization
* Legacy GSU IRQ behavior for FX1/FX2 compatibility
* FX3 completion by R15 polling
* Host/static tests with architectural integration coverage and coverage reporting
* Menu-driven SNES diagnostic ROM for hardware FX3 testing

The current cartridge and timing path are being developed around FX3. Legacy GSU1/GSU2 cycle-accurate timing has not yet been validated on hardware.

---

# How It Works

The RP2350B splits the work between both cores:

* Core 0 services the SNES cartridge bus and cross-core communication.
* Core 1 runs FX code while the GSU is active.

PIO handles the timing-sensitive bus work, including SuperFX address decoding, SNES write capture, read service, and bus-control changes.

The RP2350 provides 128 KiB of physical shared SRAM behind banks `$70-$71`. The SNES-visible window follows the installed ROM header's SRAM-size declaration and mirrors smaller SRAM sizes through that window, while the GSU retains the full physical backing. The default planar framebuffer lives in bank `$71`; SCMR and SCBR retain the normal 2/4/8bpp mode and base-selection behavior. The FX3 processor reads its FX-visible game ROM from the upper 3 MiB QSPI partition.

One canonical FX3 game ROM supplies both devices: parallel NOR stores the SNES-visible mapping, and QSPI stores the 3 MiB GSU-visible window. Smaller ROMs are mirrored into that window; a 4 MiB ROM's final MiB is SNES-only. The SNES address bus reaches the selected 8-128 Mbit NOR directly while PIO controls `/RD`, `/WR`, and `/ROM_CE`. A 128-Mbit device uses A23 and provides 16 MiB of distinct physical storage; smaller devices naturally alias bus addresses above their fitted capacity. ROM images are arranged for the detected physical layout before programming so normal CPU reads do not require live address remapping.

---

# Hardware

This firmware targets the (not yet released) NR-RetroWorks RP2350B SNES FX3 cartridge board.

The board definition is:

```text
boards/snes_fx3.h
```

CMake selects `snes_fx3` automatically and rejects other Pico board definitions.

The production routed map is implemented by the board definition, explicit bit packers, and the three-PIO/DMA front end.

## Production RP2350B Pin Groups

Signal | RP2350B GPIO | Description
--- | --- | ---
`/SNES_PRES` (Rev A/deprecated), `/SD_CS` (Rev B) | GPIO0 | Rev-A active-low presence; Rev-B SD chip-select
`/RD`, `/WR`, `/ROM_CE` | GPIO1-GPIO3 | RP2350 outputs to the single parallel ROM
`/A_OE`, `/C_OE`, `D_DIR`, `/D_OE` | GPIO4-GPIO7 | Address, control, and data translator/transceiver controls
`I_A0-I_A23` | GPIO8-GPIO31 | Routed address inputs; physical order is explicitly packed/unpacked
`/I_IRQ` (Rev A/deprecated), `SNES_PRES` (Rev B) | GPIO32 | Rev-A IRQ input; Rev-B active-high presence
`/I_CART`, `/I_RD`, `/I_WR`, `/I_RST`, `I_CLK` | GPIO33-GPIO37 | Translated console inputs
`/O_IRQ`, `/O_RST` | GPIO38-GPIO39 | Cartridge outputs through the open-drain buffer path
`I_D0-I_D7` | GPIO40-GPIO47 | Routed data bus; physical order is explicitly packed/unpacked

`/I_RD` and `/I_WR` are input-only observations of console cycles. GPIO1 `/RD` and GPIO2 `/WR` are the outputs that actually strobe the ROM. When `/C_OE` is disabled, Rev A locally drives `/I_RST` so the RP2350-side reset net never floats, then releases it before re-enabling `/C_OE`. Rev B keeps `/I_RST` input-only and drives `/O_RST` for local reset. `/O_IRQ` remains on both revisions.

Presence selects the operating mode: Rev A detects a console with GPIO0 low; Rev B detects it with GPIO32 high. Without a console, USB programming mode stops every SNES PIO watcher, disables `/A_OE`, `/C_OE`, and `/D_OE`, uses the board's local reset output, and mounts the TinyUSB mass-storage ROM loader.

## SD Card A/V Layer

Rev B adds SD card support for audio/video. Build with `-DSUPERFX3_BOARD_REVISION=B`. GPIO0 is SD CS; GPIO32 is active-high SNES_PRES.

SD is read-only: SDSC/SDHC/SDXC, FAT16/FAT32, direct volume or MBR partition, hexadecimal 8.3 filenames. Missing or unsupported media reports an error without crashing the GSU.

Control registers use banks `$00-$3F/$80-$BF`:

- Audio: `$7F00-$7F33`; HDMA tables `$6000-$6FFE`, empty terminator `$6FFF`.
- Video: `$7F40-$7F5F`; DMA pages `$71:0000` and `$71:8000`.

Audio uses `/audio/XXXX.brr`, 16 kHz mono, a 32 KiB FIFO and four HDMA pages. Set `$7F03` bit 0 for PAL. At VBlank, report SPC fill and claim one page; otherwise release with `$FF` and select the empty table. Never replay a table. STOP/errors retain ACTIVE data until release. Temporary reads retry while starvation inserts silence and reports UNDERRUN.

FMV uses 256×224 Mode-1 BG1, 4bpp/16 colors. The Pico decompresses and validates frames into two 32 KiB pages in bank `$71`. Playback requires a stopped GSU. Upload during VBlank, flip completed frames on the SPC audio clock, and drop expired frames. Disable DMA/HDMA, release the session and wait for GSU unlock before restarting it.

Requires FFmpeg/FFprobe and Pillow:

```bash
python3 src/tools/make_fx3_video.py intro.mp4 --output sd --asset-id 0001
```

Copy the generated `video/0001.fmv` and `audio/0001.brr` to SD. Default: 6 fps, 3584 DMA bytes/VBlank. Compact frames can use `--fps 20`; oversized frames are rejected. PAL: `--refresh 50 --fps 5`. `--silent` uses VBlank timing.

SD, saves and installation share QSPI ownership. SD exchanges use SRAM bus service with PIO interrupts active and GSU writes deferred. Rev B adds approximately 37 KiB of audio buffers; target linking, and stack headroom.

Addresses are mirrored in banks `$00-$3F/$80-$BF`. Access is from the SNES CPU.
All registers use byte accesses; multibyte values are little-endian.

### Audio control

| Address | Access | Register | Meaning |
|---|---|---|---|
| `$7F00` | W | Command | 0 STOP, 1 PLAY, 2 PAUSE, 3 RESUME. Reads return `$FF`. |
| `$7F01-$7F02` | R/W | Asset ID | 16-bit ID selecting `/audio/XXXX.brr`; latched when the command is written. |
| `$7F03` | R/W | Flags | Bit 0: PAL/50 Hz when set, NTSC when clear. |
| `$7F04` | R | Status | 0 idle, 1 opening, 2 priming, 3 playing, 4 draining, 5 EOF, 6 error, 7 underrun, 8 paused. |
| `$7F05` | R/W | Error | 0 none, 1 unavailable, 2 open failed, 3 read failed, 4 partial BRR block, 5 busy, 6 invalid command, 7 command queue full, 8 FIFO underrun. Write 0 to clear the code. |
| `$7F06` | R/W | HDMA page | Read offered page 0–3, or `$FF` if none. Write offered page to claim/replace ACTIVE; write `$FF` to release. |
| `$7F07` | R | First strobe | First packet sequence in the offered page; `$FF` if none. |
| `$7F08` | R/W | SPC fill | Host-reported queued 252-byte segments, clamped to 0–8. Claims are blocked at 7 or above. |
| `$7F09` | W | Event | 1 page missed, 2 packet missed, 3 SPC underrun, 4 SPC overrun. Increments the corresponding counter. |
| `$7F0A` | R | Last strobe | Last packet sequence in the offered page; `$FF` if none. |
| `$7F0B-$7F0F` | — | Reserved | Reads return `$FF`; writes ignored. |
| `$7F10-$7F13` | R | Source reads | 32-bit read-attempt count, including retries and EOF. |
| `$7F14-$7F17` | R | Source bytes | 32-bit count of bytes received. |
| `$7F18-$7F1B` | R | FIFO low-water | Minimum queued bytes since PLAY. |
| `$7F1C-$7F1F` | R | Pages built | 32-bit HDMA page count. |
| `$7F20-$7F23` | R | Pages missed | Rejected claims plus host-reported page misses. |
| `$7F24-$7F27` | R | Packets sent | Packets contained in claimed pages. |
| `$7F28-$7F2B` | R | Packets missed | Host-reported packet misses. |
| `$7F2C-$7F2F` | R | SPC underruns | Host-reported underruns. |
| `$7F30-$7F33` | R | SPC overruns | Host-reported overruns. |
| `$6000-$6FFE` | R | HDMA tables | Page bases: `$6000`, `$6400`, `$6800`, `$6C00`. |
| `$6FFF` | R | Empty table | Always returns `$00`, terminating HDMA. |

Report SPC fill before claiming a page at VBlank. Never replay the previous
table. STOP/errors retain ACTIVE until release. Strobes wrap modulo 256;
check the offered-page register before interpreting `$FF`. Counters are live,
unlatched byte reads.

### Video control

| Address | Access | Register | Meaning |
|---|---|---|---|
| `$7F40` | R/W | Command | Write 0 STOP, 1 PLAY, 2 RELEASE. Read pending command, or `$FF` when none is queued. |
| `$7F41-$7F42` | R/W | Asset ID | 16-bit ID selecting `/video/XXXX.fmv`; latched when the command is written. |
| `$7F43` | R | Status | 0 idle, 1 opening, 2 streaming, 3 EOF, 4 error. |
| `$7F44` | R | Error | 0 none, 1 unavailable, 2 busy, 3 open failed, 4 read failed, 5 invalid format, 6 bad CRC. |
| `$7F45` | R/W | Offered page | Read page 0–1, or `$FF` if none. Write offered page to claim/replace ACTIVE; write `$FF` to release ACTIVE. |
| `$7F46` | R | ACTIVE page | Currently claimed page 0–1, or `$FF` if none. |
| `$7F47` | R | Flags | Bit 0: paired audio. Bit 7: GSU locked for video. |
| `$7F48-$7F4B` | R | Frame timestamp | 32-bit presentation time in milliseconds. |
| `$7F4C-$7F4D` | R | Tile bytes | 16-bit tile-data length. |
| `$7F4E-$7F4F` | R | Frame bytes | 16-bit decoded length: tiles + 1792-byte map + 32-byte palette. |
| `$7F50-$7F53` | R | Movie duration | 32-bit duration in milliseconds. |
| `$7F54-$7F57` | R | Frame count | 32-bit total frame count. |
| `$7F58` | R | Format revision | Returns 1. |
| `$7F59-$7F5B` | — | Reserved | Reads return `$FF`; writes ignored. |
| `$7F5C-$7F5F` | R | Frame duration | 32-bit duration in milliseconds. |

Claim the offered page before reading frame metadata. DMA sources are
`$71:0000` for page 0 and `$71:8000` for page 1; data remains immutable while ACTIVE.

STOP retains ACTIVE and the GSU lock. To finish, disable DMA/HDMA, issue RELEASE,
and wait for `$7F47` bit 7 to clear. EOF means staging has finished, not that the
last frame has been displayed.

---

# Compilation

## Requirements

* Raspberry Pi Pico SDK 2.3.0 or newer
* GNU Arm Embedded Toolchain with `arm-none-eabi-gcc`, `arm-none-eabi-g++`, and `arm-none-eabi-size`
* CMake 3.13 or newer
* Python 3.10 or newer
* GNU `g++` for the host/static test suite
* `gcov` for optional coverage reporting
* cc65 with `ca65` and `ld65` for the diagnostic ROM
* ffmpeg/pillow for a/v support

Set `PICO_SDK_PATH` if needed:

```bash
export PICO_SDK_PATH="$HOME/pico/pico-sdk"
```

### External Libraries

SuperFX3 uses the following controller-side library:

* **TinyUSB** for USB device and mass-storage support.

External libraries are supplied through the Pico SDK checkout.

### A/V Support

On Ubuntu/Debian WSL, install the required A/V support tools via:

```bash
sudo apt install ffmpeg
python3 -m pip install Pillow
```

## Building

Configure and build from the repository root:

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

The main linked firmware output is:

```text
build/superfx3.elf
```

The Pico SDK also generates `.bin`, `.hex`, `.uf2`, map, and disassembly outputs. The raw `.bin` is used when creating a combined QSPI image. The build reports firmware-partition use and static `data+BSS` SRAM use after linking.

---

# FX3 Diagnostic ROM

![SuperFX3 Diagnostic ROM](testrom/testrom.png)  
`testrom/` contains a SNES diagnostic application for exercising the FX3 implementation. The 65816 owns the menu, test setup, timeouts, validation, and display while small GSU kernels perform the operations under test.

Build it with:

```bash
python3 testrom/build.py
```

Or through CMake:

```bash
cmake --build build --target fx3_testrom
```

The build produces:

File | Purpose
--- | ---
`fx3_test.sfc` | Authoritative 3 MiB canonical FX3 diagnostic ROM
`fx3_test_fxrom.bin` | Compact linked GSU kernels for development/debugging
`fx3_test_fxrom_partition.bin` | 3 MiB FX-visible ROM derived from the canonical `.sfc`
`fx3_test_parallel_rom.bin` | 16 MiB parallel NOR image derived from the canonical `.sfc`
`fx3_test_manifest.json` | Hashes, sizes, QSPI offset, GSU entry points, and matched-pair ID

The suite covers register access, STOP, repeated START/STOP, shared RAM, ALU behavior, FX ROM reads, ROM-to-ALU-to-RAM execution, PLOT, RPIX, CLEAR, and current FX3 C2P behavior. Graphics tests validate shared RAM before showing the result on screen.

Run source/layout checks without cc65:

```bash
python3 testrom/build.py --check
```

The native `ca65`/`ld65` build has been verified, and the 65816 supervisor UI has been smoke-tested in Snes9x. Hardware FX3 validation is still pending.

See `testrom/README.md` for the test ABI and programming-image details.

---

# SNES Parallel ROM Image

The SNES CPU uses one parallel ROM on the full 24-bit address bus.

Create a programming image with:

```bash
python3 src/tools/make_snes_rom_image.py \
    path/to/game.sfc \
    build/game_parallel_rom.bin \
    --map lorom \
    --chip-size-mbit 128
```

The device may be 8, 16, 32, 64, or 128 Mbit. Pass `--chip-size-mbit` to select it explicitly. When omitted, the tool checks a validated internal SNES header, the normalized file size, and the selected map's direct-bus alias requirements, then chooses the smallest supported device that satisfies all of them. Header ROM-size codes use the standard `2^N` KiB encoding through `$0E`, which denotes 128 Mbit. Raw images infer capacity only from an exact supported file size. Ambiguous, conflicting, or oversized inputs are rejected instead of defaulting to 128 Mbit.

The output size always equals the selected device capacity. Raw images can use every byte uniquely, including both A23 halves of a 128-Mbit device. Mapped formats duplicate bytes only where the selected SNES mapping defines a mirror. The packer does not mask A23 on a 128-Mbit target.

Supported mappings are `lorom`, `hirom`, `exlorom`, `exhirom`, `fx3`, `superfx-extended`, and `raw`. A 512-byte copier header is removed automatically for mapped SNES ROMs. `superfx-extended` retains the unrelated 11 MiB Snes9x layout; production FX3 uses `fx3`.

`--map fx3` accepts canonical ROMs up to 4 MiB (type `$17`/`$18`) or a validated 8 MiB production dump, optionally followed by 256 erased bytes. Both copies of every 32 KiB block in the dump's first 4 MiB must match the first 2 MiB of its canonical second half. On a 128-Mbit device, the 16 MiB image explicitly programs both A23 halves: duplicated 32 KiB blocks at `$00-$3F`/`$80-$BF`, the shared linear window at `$40-$6F`, and the full canonical window at `$C0-$FF`. These repetitions are FX3 CPU-map mirrors, not a physical A23 alias. Physical bytes beneath `$70/$71` SRAM never replace its CPU overlay; `$7E/$7F` remain WRAM. Add `--fx-rom-output build/game_fxrom.bin` to generate the complete 3 MiB QSPI FX window, mirroring smaller ROMs with SNES-style mirroring.

Programming images are padded to the selected physical ROM capacity with `0xFF`. The default FX3/raw profile retains full SRAM banks `$70-$71`. Installed ExLoROM instead uses the low halves of `$70-$7D`/`$F0-$FD` for SRAM; their upper halves remain ROM. ExHiROM uses `$20-$3F`/`$A0-$BF:$6000-$7FFF` for SRAM, with FX3 registers still available in `$00-$1F`/`$80-$9F`. These extended profiles cannot simultaneously provide the original full-bank `$71` framebuffer window. Both extended profiles allow ROM reads in `$72-$7D`.

Mapped 128-Mbit images carry a 16-byte `S3MP` descriptor with the map, canonical ROM size, declared SRAM size, and cartridge type at physical `0x7E0000`, a SNES WRAM address that cannot expose cartridge ROM. Both the offline packer and USB installer emit it, and firmware reads it before enabling console translators. Smaller devices and raw images also use a validated bus-visible SNES header for SRAM/type metadata; images without either retain the default FX3 profile.

## USB drag-and-drop programming

With the cartridge powered only by USB (presence inactive), the firmware exposes a FAT12/FAT16 drive named `SUPERFX3`. Copy one LoROM, HiROM, ExLoROM, ExHiROM, or FX3 `.sfc`/`.smc` source image that fits the installed device, or a ready-to-flash `.rom`/`.bin` image exactly matching its physical capacity. Then **safely eject the drive, keeping USB power connected until `/O_IRQ` is released**. Eject finalizes the FAT chain and starts installation; an ordinary cache flush or temporary file size does not. The loader reads CFI geometry, accepts uniform 128 KiB-sector devices from 8 through 128 Mbit, constructs the direct cartridge-bus image, and programs/verifies the complete detected capacity. A safe eject is not yet confirmation that NOR programming has finished.

USB also accepts canonical FX3 `.sfc`/`.smc` files and validated production dumps, including the optional 256-byte erased trailer. FX3 installation programs/verifies the full 3 MiB FX-visible window in QSPI at `0x100000`, then the parallel image, and commits the descriptor last. SDK flash lockout parks Core 1 and disables local interrupts during each QSPI sector write. Firmware and saves remain untouched. Ordinary ROM and raw-image uploads still change only parallel NOR. Interrupting a two-device installation can leave a mismatched pair; re-upload the ROM to recover.

Data sectors are retained by physical cluster even when they precede directory or FAT updates. The volume size follows the detected parallel flash capacity and includes 16 KiB of temporary SRAM for allocation overhead. Repeated sector writes use a read/modify/erase/program cache. Fragmented chains are reordered without losing unread pages; heavy fragmentation can substantially increase programming time. One ROM per mount is supported. Copying begins a destructive replacement of the previous ROM; disconnecting or losing power partway through requires uploading again. This is not an atomic firmware-update mechanism.

A 512-byte copier header is selected only when the shifted SNES header validates better than the unshifted candidate. Detection checks checksum/complement, map mode, reset vector, title plausibility, and FX3 type/size where applicable. The filename extension and size remainder alone never cause a shift. Raw `.rom`/`.bin` images remain byte-exact.

Parallel-flash writes use the byte-mode AMD/JEDEC command protocol with `AAA/555` unlock addresses, DQ7 data polling, DQ5 timeout handling, 128 KiB sector erase, and read-back verification. Mapped uploads use the parallel NOR itself as overlap-safe temporary storage. `/O_IRQ` remains asserted throughout receive/program work and is released on every completion or failure path. If a powered SNES appears, programming aborts before the firmware reconnects the console bus.

---

# FX3 QSPI Image

The production W25Q32 QSPI device uses one fixed three-part layout:

```text
0x000000-0x07BFFF  RP2350 firmware
0x07C000-0x0FFFFF  append-only SRAM save journal
0x100000-0x3FFFFF  FX-visible game ROM
```

Create a combined image with:

```bash
python3 src/tools/make_fx3_qspi_image.py \
    build/superfx3.bin \
    path/to/fx3_rom.bin \
    build/superfx3_qspi.bin
```

The tool enforces the fixed 4 MiB QSPI layout: 496 KiB firmware, 528 KiB saves, and 3 MiB FX ROM. Save and unused prepared-image space are filled with `0xFF`. Pass `--fx3-rom` to derive the complete 3 MiB FX-visible window from a canonical or production-dump FX3 ROM. Smaller games are mirrored; the fourth MiB of a 4 MiB game is excluded.

The save journal, starting at `0x07C000`, holds four 132 KiB slots, each containing a header and complete 128 KiB SRAM snapshot. Payload is written first, with the `SFX3`/`CMIT` header committed last. Boot restores the latest/newest CRC-valid record. Reusing a slot preserves the latest valid save until its replacement succeeds.

`ALT3; STOP` drains pending memory activity, stops the GSU, and saves from Core 1 while Core 0 is parked. Only SRAM/ROM-safe code executes while XIP is unavailable. After verification, XIP and Core 0 resume, R15 becomes zero, and the GSU stays stopped. Official FX3 uses R15 polling; compatibility profiles may assert a completion IRQ. Saves require no busy IRQ, USB connection, or save-and-continue behavior.

Failure leaves the GSU stopped with nonzero R15 and no new completion IRQ. Firmware exposes the result through `save_failed` and `qspi_save_last_ok()`; guest software should use a timeout and explicitly restart after recovery. Reset/disconnect saves remain serialized fallbacks. Cartridge type `$17` disables persistence, while `$18` enables it; a disabled SAVE_AND_STOP is a successful no-op. The journal format remains a complete 128 KiB snapshot even when the CPU-visible SRAM declaration is smaller. Saves never modify firmware, FX ROM, or parallel ROM.

**Wait for save completion before removing power.** Sudden power loss cannot trigger a reliable save. The journal stores one cartridge-wide SRAM image, without per-game slots.

---

# Testing

Run the host/static suite from the repository root:

```bash
bash src/tests/run_tests.sh
```

This runs the processor and opcode tests, architectural tests, bus integration simulation, synchronization and backend tests, ROM packing tests, PIO static checks, and strict production-source stub links.

For coverage:

```bash
bash src/tests/run_coverage.sh
```

The report is written to:

```text
src/build/coverage/coverage_report.txt
```

Current coverage gates are:

* Portable `fx/*.cpp` line coverage of at least 95%
* Host-testable core, sync, and backend line coverage of at least 90%
* Branch alternatives taken at least once of at least 75%

The current suite passes. Host tests catch processor and integration problems, but real SNES timing, GPIO behavior, DMA timing, interrupt latency, and electrical behavior still require hardware validation.

---

# Source Layout

Path | Description
--- | ---
`boards/snes_fx3.h` | RP2350B board definition and cartridge GPIO map
`src/fx/` | SuperFX processor core, opcodes, registers, memory, and graphics
`src/storage/` | QSPI layout and append-only SRAM save-journal format
`src/platform/rp2350/` | RP2350 backend, synchronization, SNES bus, PIO, and DMA support
`src/tests/` | Host tests, PIO checks, SDK stubs, and coverage tools
`src/tools/` | ROM and QSPI image utilities
`testrom/` | SNES FX3 diagnostic ROM and GSU test kernels
`src/main.cpp` | Firmware startup, shared RAM, multicore setup, and main service loop

---

# Current Status

Remaining hardware work includes:

* Real SNES bus timing and logic analyzer verification
* PIO timing under cartridge load
* Cross-core queue depth and worst-case service latency
* CPU-write ordering and shared-state publication against hardware or trusted traces
* Production SRAM/linker-map accounting
* Real-board SAVE_AND_STOP/reset/disconnect saves, Core-0 lockout latency, SNES bus behavior during lockout, USB host compatibility, and programming time
* Final FX3 behavior checks against hardware and trusted traces
* Legacy GSU1/GSU2 timing if those modes remain supported

Expect things to move around during hardware validation.

---

# Credits

Portions of the SuperFX processor implementation are based on the GSU implementation from [Mesen Community Edition](https://github.com/nesdev-org/MesenCE), licensed under the GNU GPL.

Special thanks to Randy Linden, Sunlit, and kandowantu.

Dedicated to author's career mentors Rebecca Heineman and Jennell Jaquays.

---

# License

SuperFX3 is licensed under the GNU General Public License, version 3 or later.

See `LICENSE` for the complete license text.
