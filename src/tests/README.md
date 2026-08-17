# SuperFX3 host test suite

## Test groups

- `core_tests.cpp` keeps the fixed and randomized 8bpp bit-transpose vectors on the real PLOT/pixel-cache path, checks the 20-tile FX3 layout stride, RON/RAN bypass, R15/register mapping, and the complete legacy blocked-ROM byte pattern.
- `opcode_tests.cpp` exercises every opcode dispatch entry, then checks representative control, ALU, data, graphics, cache, and timing paths with explicit results.
- `fx_core_sanity.cpp` checks the current FX3 behavior: 8bpp PLOT/pixel-cache behavior, all three clear regions, MERGE command dispatch, linear QSPI ROM backend access and GSU ROM mapping, VCR, R15/STOP behavior, and simultaneous FX3 CPU ROM/RAM visibility.
- `architectural_tests.cpp` runs small FX3 programs only through the public CPU/run interfaces, then checks that completion is not visible until delayed side effects have landed and that results are stable across different core-1 execution budgets.
- `bus_integration_tests.cpp` runs the real SNES bus, PIO frontend, sync layer, and FX core against stateful GPIO/PIO/DMA/IRQ stubs. It injects captured writes, reads, reset interrupts, legacy ROM ownership changes, physical ROM transactions, pause/resume, and both one-ROM and two-ROM configurations.
- `sync_tests.cpp` exercises the cross-core command queue, snapshots, reset retry, STOP handoff, IRQ acknowledgement, queue-full behavior, and legacy GSU ownership notifications without RP2350 hardware.
- `register_backend_tests.cpp` covers register read/write side effects and the host-testable RP2350 backend callbacks and bounds checks.
- `pio_static_tests.py` interprets the PIO source and exhaustively checks routing/selector decisions, instruction-memory use, encodable `SET` immediates, write capture packing, WAIT GPIOs against `boards/snes_fx3.h`, the C++/PIO IRQ contract, and the custom-board CMake setup.
- `packer_tests.py` verifies the combined QSPI image layout, padding, 4/8 MiB placement, and invalid-layout rejection.
- `snes_rom_image_tests.py` checks LoROM, HiROM, ExLoROM, and ExHiROM address mapping plus the 16 MiB parallel-flash bus image.

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

The coverage build instruments the portable FX core plus `fx_sync.cpp` and `fx_backend.cpp`. `snes_bus.cpp` and `snes_pio.cpp` are now dynamically exercised by the stateful bus-integration harness, but they are deliberately left out of the coverage percentage. A host-side hardware model is useful for checking software sequencing and architectural contracts, but a high line percentage there would still say nothing about real GPIO timing, PIO instruction timing, FIFO/DMA concurrency, interrupt latency, or SNES electrical margins. PIO routing is also checked independently by the source-level interpreter.

`main.cpp` remains strict compile/link checked rather than dynamically covered.

Current gates are intentionally high enough to catch meaningful test regressions without pretending host coverage closes hardware timing:

- portable `fx/*.cpp` line coverage: at least 95%
- host-testable core/sync/backend line coverage: at least 90%
- branch alternatives taken at least once: at least 75%

The generated report is written to `build/coverage/coverage_report.txt`.

Coverage is evidence that code paths were exercised, not proof that the RP2350 meets the SNES bus timing. Hardware trace tests remain a separate requirement.
