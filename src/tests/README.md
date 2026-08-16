# SuperFX3 host test suite

## Test groups

- `core_tests.cpp` keeps the fixed and randomized 8bpp bit-transpose vectors on the real PLOT/pixel-cache path, checks the 20-tile FX3 layout stride, RON/RAN bypass, R15/register mapping, and the complete legacy blocked-ROM byte pattern.
- `opcode_tests.cpp` exercises every opcode dispatch entry, then checks representative control, ALU, data, graphics, cache, and timing paths with explicit results.
- `fx_core_sanity.cpp` checks the current FX3/PDF behavior: 8bpp PLOT/pixel-cache behavior, all three clear regions, MERGE command dispatch, QSPI logical ROM mapping, VCR, R15/STOP behavior, and simultaneous FX3 CPU ROM/RAM visibility.
- `sync_tests.cpp` exercises the cross-core command queue, snapshots, reset retry, STOP handoff, IRQ acknowledgement, and legacy GSU ownership notifications without RP2350 hardware.
- `register_backend_tests.cpp` covers register read/write side effects and the host-testable RP2350 backend callbacks and bounds checks.
- `pio_static_tests.py` interprets the PIO source and exhaustively checks routing/selector decisions, instruction-memory use, write capture packing, WAIT GPIO ranges, and the C++/PIO IRQ contract.
- `packer_tests.py` verifies the combined QSPI image layout, padding, 4/8 MiB placement, and invalid-layout rejection.

Run the complete host/static suite with:

```sh
./tests/run_tests.sh
```

`run_static_tests.sh` is retained as a compatibility wrapper for the older test bundle.

## Coverage

Run:

```sh
./tests/run_coverage.sh
```

The coverage build instruments the portable FX core plus the host-testable `fx_sync.cpp` and `fx_backend.cpp`. It deliberately does **not** report dynamic host coverage for `main.cpp`, `snes_bus.cpp`, or `snes_pio.cpp`: Pico SDK stubs cannot model real GPIO, PIO, DMA, interrupt latency, or SNES electrical timing well enough for that percentage to be meaningful. Those files are still strict compile/link checked, while PIO routing is checked by the source-level interpreter.

Current gates are intentionally high enough to catch meaningful test regressions without pretending host coverage closes hardware timing:

- portable `fx/*.cpp` line coverage: at least 95%
- host-testable core/sync/backend line coverage: at least 90%
- branch alternatives taken at least once: at least 75%

The generated report is written to `build/coverage/coverage_report.txt`.

Coverage is evidence that code paths were exercised, not proof that the RP2350 meets the SNES bus timing. Hardware trace tests remain a separate requirement.
