# 0.10.0 review corrections

This changed-files bundle is cumulative against local Git HEAD `cbd9a01`. It
includes the earlier uncommitted hardware/storage port and this correction pass.
Extract over the corresponding repository root. No commit or remote push was made.
The existing minor-version bump to 0.10.0 is retained.

## Corrected

- PIO releases all eight data directions with `OUT PINDIRS`, not an invalid
  eight-pin `SET` group. The host stub now enforces the SDK's five-pin SET limit.
- USB retains sectors independently of directory/FAT write order, handles growing
  files and fragmented chains, and finalizes only on safe eject. Sector caching
  supports repeated writes that require NOR bits to change from zero to one.
- Explicit abort, USB unmount, and console-mode transitions release storage busy
  and stop in-flight programming. FX IRQ and storage busy have separate owners.
- Extended maps no longer lose ROM behind the `$72-$7D` rejection or fixed
  full-bank SRAM decoding. Installed-map metadata survives reboot in unused
  parallel-ROM WRAM address space. The offline image packer emits the same metadata.
- Journal wrap erases only the next slot, retaining the newest committed save.
  New headers encode `SFX3` correctly; earlier `SXF3` records remain readable.
- Production QSPI callbacks, boot restore, and reset/disconnect saves are wired
  through the SDK flash-safe mechanism. The QSPI partition layout is unchanged.
- Valid 32 KiB and 64 KiB images are accepted, including 512-byte copier headers.

## Operating notes

Copy one ROM and safely eject. Keep USB power present until programming finishes
and `/O_IRQ` releases. Eject queues installation; it does not mean NOR programming
has finished. The volume is disposable upload staging, not general-purpose file
storage. Writes replace the old parallel image in place; an aborted transfer or
power failure requires uploading again. Fragmented transfers can be much slower.

Press console reset and allow the save to finish before removing power. Saves also
occur after SNES disconnect if USB power remains. Sudden loss of both supplies
cannot trigger a reliable save. This pass does not add a live-game save handshake
or per-game save identification.

ExLoROM/ExHiROM use their documented mapper-specific SRAM windows. They cannot
simultaneously reserve the original entire `$71` framebuffer bank for FX3 SRAM.
The raw/default FX3 profile preserves the original full-bank SRAM map. Standard
mapped sources support up to 8 MiB, raw physical images support all 128 Mbit
(16 MiB), and the offline packer still supports the 11 MiB extended SuperFX map.

## Verification

- Complete host/static suite: PASS, including the strict production host link.
- Command-aware NOR tests: headered small ROM, fragmented source ordering,
  repeated sector writes, cancellation, and full 16 MiB raw installation: PASS.
- Production QSPI wrapper with emulated SDK safety/storage: restore, reset and
  disconnect triggers, failure cleanup, and partition bounds: PASS.
- Every source page mapped by 8 MiB ExLoROM/ExHiROM has a runtime-readable alias: PASS.
- Pico SDK 2.3.1 pioasm assembly: PASS. Instruction use: PIO0 6, PIO1 18, PIO2 23.
- Git whitespace check: PASS.

No ARM compiler/CMake toolchain was available for a fresh target firmware build.
The ZIP contains source changes, not a hardware-validated firmware binary. Real
SNES timing, host USB compatibility/timeouts, actual flash timings, and the target
linker's SRAM budget still need board/toolchain validation.
