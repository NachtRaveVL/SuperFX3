# SuperFX3 host test suite

## Test groups

- `core_tests.cpp` keeps the fixed and randomized 8bpp bit-transpose vectors on the real PLOT/pixel-cache path, checks the 20-tile FX3 layout stride, RON/RAN bypass, R15/register mapping, and the complete legacy blocked-ROM byte pattern.
- `opcode_tests.cpp` exercises every opcode dispatch entry, then checks representative control, ALU, data, graphics, cache, and timing paths with explicit results.
- `fx_core_sanity.cpp` checks the current FX3 behavior: 8bpp PLOT/pixel-cache behavior, 4bpp PLOT/RPIX with non-default SCBR, all three clear regions, plain/ALT1/unassigned-prefix MERGE dispatch, linear QSPI ROM backend access and GSU ROM mapping, VCR, R15/STOP behavior, and simultaneous FX3 CPU ROM/RAM visibility.
- `architectural_tests.cpp` runs small FX3 programs only through the public CPU/run interfaces, then checks that completion is not visible until delayed side effects have landed and that results are stable across different core-1 execution budgets.
- `bus_integration_tests.cpp` runs the real SNES bus, routed-bit helpers, PIO frontend, sync layer, and FX core against stateful GPIO/PIO/DMA/IRQ stubs. It injects captured writes, reads, reset interrupts, single-ROM physical transactions, pause/resume, console-presence transitions, local `/I_RST` ownership, input-only `/I_RD`/`/I_WR`, and `/O_IRQ` behavior.
- `sync_tests.cpp` exercises the cross-core command queue, snapshots, reset retry, STOP handoff, IRQ acknowledgement, queue-full behavior, and legacy GSU ownership notifications without RP2350 hardware.
- `register_backend_tests.cpp` covers register read/write side effects and the host-testable RP2350 backend callbacks and bounds checks.
- `save_journal_tests.cpp` covers empty media, append/restore ordering, CRC fallback, interrupted uncommitted slots, flash failures, erase-on-wrap behavior, destination-slot preservation, and finalized header validation. The journal has no IRQ API.
- `pio_static_tests.py` checks the exact routed production pin map, PIO instruction-memory use, encodable `SET` immediates, input-strobe WAIT GPIOs, DMA bridge endpoints, single-ROM controls, C++/PIO contracts, and custom-board CMake setup.
- `packer_tests.py` verifies the fixed 4 MiB W25Q32 firmware/save/FX-ROM layout, erased journal region, padding, and rejection of alternate sizes or offsets.
- `storage_separation_tests.py` prevents the QSPI save journal and external parallel game-ROM installer from referencing each other's storage paths.
- `snes_rom_image_tests.py` checks LoROM, HiROM, ExLoROM, ExHiROM, extended SuperFX, and raw mapping into the single parallel ROM through 128 Mbit.
- `parallel_rom_programmer_tests.cpp` locks x8 AMD/CFI capacity detection from 8 through 128 Mbit, `AAA/555` program and erase commands, DQ7 polling, DQ5 failure, and read-array reset sequences.
- `snes_rom_layout_tests.cpp` validates automatic LoROM/HiROM/ExLoROM/ExHiROM detection, bus mapping through 8 MiB, content-based 512-byte copier-header stripping, and installed SRAM/type metadata.
- `usb_installer_plan_tests.py` proves that the in-place 8-128 Mbit installer does not erase unread mapped source pages and locks the mapped/raw capacity contracts.
- `usb_rom_volume_tests.cpp` exercises the generated FAT16 boot sector, mapped and raw upload types, cluster-to-file mapping, completion tracking, and MSC readback.
- `usb_block_order_tests.cpp` covers data-before-directory/FAT, growing files, cache flush versus eject, fragmented chains, incomplete/cyclic chains, and staging bounds.
- `snes_rom_installer_tests.cpp` uses a command-aware NOR emulator for a fragmented headered 32 KiB image, zero-to-one sector rewrites, full-capacity 8/16/32/64/128 Mbit raw images, busy-owner rejection, and cancellation between writes.
- `save_stop_tests.cpp` runs ALT3+STOP through the core and production QSPI wrapper against emulated flash, covering retired RAM/plot writes, parked-core snapshot/commit, completion ordering, no busy IRQ, failure/retry, exact instruction gating, and prefix cleanup. The SDK model does not prove physical multicore or bus timing.
- `qspi_save_integration_tests.cpp` runs the production save wrapper with emulated SDK flash safety and storage, checking reset/disconnect triggers, non-battery suppression, unchanged-save suppression, boot restore, failure cleanup, and partition bounds.
- Journal regressions retain the prior save after a failed wrap and check `SFX3` byte order plus legacy compatibility. Bus tests check every mapped 8 MiB extended-ROM source page has a readable alias and verify independent core/storage IRQ ownership.

Run the complete host/static suite with:

```sh
bash tests/run_tests.sh
```

Calling the script through `bash` avoids executable-bit issues when the tree is unpacked on Windows/WSL. The runner deletes stale test binaries, recompiles each C++ target, and labels the compile (`CXX`) and run (`RUN`) stages separately.

`run_static_tests.sh` is retained as a compatibility wrapper for the older test bundle.

## Coverage

Run:

```sh
bash tests/run_coverage.sh
```

The coverage build instruments the portable FX core plus `fx_sync.cpp` and `fx_backend.cpp`. `snes_bus.cpp` and `snes_pio.cpp` are now dynamically exercised by the stateful bus-integration harness, but they are deliberately left out of the coverage percentage. A host-side hardware model is useful for checking software sequencing and architectural contracts, but a high line percentage there would still say nothing about real GPIO timing, PIO instruction timing, FIFO/DMA concurrency, interrupt latency, or SNES electrical margins. PIO routing also has independent pin-map, instruction-memory, WAIT-strobe, and DMA-endpoint checks.

`main.cpp` remains strict compile/link checked rather than dynamically covered.

Current gates are intentionally high enough to catch meaningful test regressions without pretending host coverage closes hardware timing:

- portable `fx/*.cpp` line coverage: at least 95%
- host-testable core/sync/backend line coverage: at least 90%
- branch alternatives taken at least once: at least 75%

The generated report is written to `build/coverage/coverage_report.txt`.

Coverage is evidence that code paths were exercised, not proof that the RP2350 meets the SNES bus timing. Hardware trace tests remain a separate requirement.
