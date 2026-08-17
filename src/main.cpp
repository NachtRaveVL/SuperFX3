/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 *
 * Portions of this software are based on MesenCE's GSU implementation (GPLv3).
 *
 * Special thanks to Randy Linden and kandowantu.
 * Dedicated to Rebecca Heineman and Jennell Jaquays.
 */

#include <atomic>

#include "platform/rp2350/snes_bus.h"
#include "fx/fx_core.h"
#include "platform/rp2350/fx_backend.h"
#include "platform/rp2350/fx_sync.h"

#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico.h"
#include "pico/stdlib.h"

static constexpr uint32_t FX3_SYS_CLOCK_HZ = 150000000u; ///< Required RP2350B system clock for FX3.
static constexpr uint32_t RAM_SIZE = 128u * 1024u; ///< Shared SRAM backing SuperFX banks $70-$71.

static_assert(std::atomic<uint8_t>::is_always_lock_free,
              "Shared SuperFX RAM requires lock-free byte atomics on RP2350.");
static_assert(sizeof(std::atomic<uint8_t>) == sizeof(uint8_t),
              "Shared SuperFX RAM assumes one byte of storage per atomic byte.");

static SuperFx fx;
alignas(4) static std::atomic<uint8_t> g_ram[RAM_SIZE];

static Rp2350FxBackendContext g_fx_backend_context {
    nullptr, 0, RAM_SIZE,
    g_ram,
    fx3_qspi_rom_read, snes_irq_write
};

// Runs the SuperFX execution service continuously on RP2350 core 1.
void __not_in_flash_func(core1_main)() {
    while (true) {
        if (!fx_sync_core1_service())
            tight_loop_contents();
    }
}

// Initializes shared RAM, the SuperFX core, PIO bus service, and the second core.
int main() {
    // FX3 Technical Specifications v1.0 identifies the cartridge RP2350B as
    // running at 150 MHz. The PIO timing audit and FX3 throughput assumptions
    // are tied to that clock, so fail closed if the production clock setup drifts.
    if (clock_get_hz(clk_sys) != FX3_SYS_CLOCK_HZ)
        panic("FX3 requires clk_sys = 150 MHz");

    for (auto& byte : g_ram)
        std::atomic_init(&byte, static_cast<uint8_t>(0));

    snes_bus_init();

    // FX3 uses the RP2350's primary QSPI flash for its private ROM image. The
    // image occupies a reserved partition above the firmware and is read directly
    // through the RP2350 XIP window.
    if (!fx3_qspi_rom_init(g_fx_backend_context))
        panic("FX3 firmware overlaps the reserved QSPI ROM partition");

    FxBackend backend = fx_backend_create(&g_fx_backend_context);

    fx.init(fx3_config, backend);
    fx_sync_init(fx, backend);
    snes_bus_start(fx);

    multicore_launch_core1(core1_main);

    while (true) { // core0 loop
        snes_bus_service();
        tight_loop_contents();
    }
}
