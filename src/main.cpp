/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 *
 * Portions of this software derived from Mesen emulator.
 * Special thanks to Randy Linden and kandowantu.
 * Dedicated to Rebecca Heinemann and Jennel Jacquays.
 */

#include "cart/cart_bus.h"
#include "fx/fx_core.h"
#include "rp2350/fx_bus.h"

#include "pico/multicore.h"
#include "pico/platform/sections.h"
#include "pico/stdlib.h"

// 64 Mbit external ROM = 8 MiB.
static constexpr uint32_t ROM_SIZE = 8u * 1024u * 1024u;

// SuperFX banks $70-$71 provide 128 KiB of addressable RAM.
// This fits comfortably inside the RP2350's on-chip SRAM.
static constexpr uint32_t RAM_SIZE = 128u * 1024u;

static SuperFx fx;
alignas(4) static uint8_t g_ram[RAM_SIZE] {};

static Rp2350FxBusContext g_fx_bus_context {
    ROM_SIZE,
    RAM_SIZE,
    g_ram
};

// -----------------------------------------------------------------------------
// Dedicated SuperFX execution core
// -----------------------------------------------------------------------------
void __not_in_flash_func(core1_main)() {
    while (true) {
        if (!fx.running()) {
            tight_loop_contents();
            continue;
        }

        fx.run_unlimited(256);
    }
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
int main() {
    cart_bus_init(CART_PINS);

    FxBus bus = fx_bus_create(&g_fx_bus_context);
    fx.init(fx3_config, bus);

    multicore_launch_core1(core1_main);

    while (true) {
        cart_bus_service(fx);
    }
}
