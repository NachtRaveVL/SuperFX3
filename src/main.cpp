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
#include "platform/rp2350/parallel_rom_gpio.h"
#include "platform/rp2350/snes_pio.h"
#include "platform/rp2350/qspi_save.h"
#include "usb/usb_rom_loader.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/flash.h"
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
static std::atomic<bool> g_core1_ready {false};

static Rp2350FxBackendContext g_fx_backend_context {
    nullptr, 0, RAM_SIZE,
    g_ram,
    fx3_qspi_rom_read, snes_irq_write
};

// Runs the SuperFX execution service continuously on RP2350 core 1.
void __not_in_flash_func(core1_main)() {
    if (!flash_safe_execute_core_init())
        panic("Unable to initialize core-1 flash safety");
    g_core1_ready.store(true, std::memory_order_release);
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
    const ParallelRomBus parallel = parallel_rom_gpio_bus();
    snes_pio_set_rom_map(snes_rom_installed_map({parallel.context, parallel.read}));

    // FX3 uses the RP2350's primary QSPI flash for private FX code and data.
    // This fixed partition is separate from the external parallel game ROM and
    // is read directly through the RP2350 XIP window.
    if (!fx3_qspi_rom_init(g_fx_backend_context))
        panic("FX3 firmware overlaps the reserved QSPI save partition");
    qspi_save_init(g_ram);

    FxBackend backend = fx_backend_create(&g_fx_backend_context);
    backend.save = qspi_save_now;

    fx.init(fx3_config, backend);
    fx_sync_init(fx, backend);
    snes_bus_start(fx);

    usb_rom_loader_init();
    usb_rom_loader_set_enabled(snes_bus_usb_mode());

    // Core 1 may write QSPI during SAVE_AND_STOP. Core 0 must participate in
    // SDK lockout too; its ordinary bus-service path is not XIP-independent.
    if (!flash_safe_execute_core_init())
        panic("Unable to initialize core-0 flash safety");
    multicore_launch_core1(core1_main);
    while (!g_core1_ready.load(std::memory_order_acquire))
        tight_loop_contents();

    while (true) { // core0 loop
        // Cancel and release storage ownership before reconnecting translators.
        if (!gpio_get(SNES_PRES_N_PIN))
            usb_rom_loader_set_enabled(false);
        snes_bus_service();
        qspi_save_task();
        usb_rom_loader_set_enabled(snes_bus_usb_mode());
        usb_rom_loader_task();
        tight_loop_contents();
    }
}
