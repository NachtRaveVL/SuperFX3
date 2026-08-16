/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include <atomic>

#include "snes_bus.h"
#include "snes_pio.h"
#include "fx_sync.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/platform/sections.h"
#include "pico/stdlib.h"

static std::atomic<bool> g_bus_request {false};
static std::atomic<bool> g_bus_granted {false};
static SnesBusPins g_pins {};
static SuperFx* g_fx = nullptr;
static uint32_t g_rom_access_cycles = 0;
static bool g_started = false;

// Extracts D0-D7 from one RP2350B GPIO snapshot.
static inline uint8_t snes_read_data(uint64_t gpio) {
    return static_cast<uint8_t>((gpio >> SNES_DATA_BASE) & 0xFFu);
}

// Places a 24-bit SNES address into the two non-contiguous GPIO address groups.
static inline uint64_t snes_address_output_value(uint32_t address) {
    return static_cast<uint64_t>(address & 0xFFFFu) |
           (static_cast<uint64_t>((address >> 16) & 0xFFu) << SNES_ADDR_HI_BASE);
}

// Returns true only when neither SNES address bus is in an active transaction.
static inline bool snes_bus_idle() {
    // FIXME: Replace this point-in-time idle sample with a bus handoff that stays valid through the ownership change.
    // A new SNES cycle can begin immediately after this check, and the synchronized GPIO inputs can lag the physical
    // strobes. Prove a safe handoff window from SNES timing, or add hardware/PIO arbitration that guarantees one.
    return gpio_get(g_pins.rd_n) && gpio_get(g_pins.wr_n) && gpio_get(g_pins.romsel_n) &&
           gpio_get(g_pins.pard_n) && gpio_get(g_pins.pawr_n);
}

void snes_bus_init(const SnesBusPins& pins) {
    g_pins = pins;
    g_bus_request.store(false, std::memory_order_relaxed);
    g_bus_granted.store(false, std::memory_order_relaxed);

    for (uint8_t pin = SNES_ADDR_LO_BASE; pin < SNES_ADDR_LO_BASE + 16; pin++) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_IN);
    }
    for (uint8_t pin = SNES_CTRL_BASE; pin < SNES_CTRL_BASE + 16; pin++) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_IN);
    }
    for (uint8_t pin = SNES_ADDR_HI_BASE; pin < SNES_ADDR_HI_BASE + 8; pin++) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_IN);
    }
    for (uint8_t pin = SNES_DATA_BASE; pin < SNES_DATA_BASE + 8; pin++) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    // Program the safe values before turning the external bus transceiver around.
    gpio_put(g_pins.bus_oe_n, BUS_DISABLE);
    gpio_set_dir(g_pins.bus_oe_n, GPIO_OUT);
    gpio_put(g_pins.data_dir, DATA_DIR_IN);
    gpio_set_dir(g_pins.data_dir, GPIO_OUT);
    gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
    gpio_set_dir(g_pins.rom_oe_n, GPIO_OUT);
    gpio_put(g_pins.irq_n, 1);
    gpio_set_dir(g_pins.irq_n, GPIO_OUT);

    gpio_pull_up(g_pins.rd_n);
    gpio_pull_up(g_pins.wr_n);
    gpio_pull_up(g_pins.romsel_n);
    gpio_pull_up(g_pins.reset_n);
    gpio_pull_up(g_pins.wramsel_n);
    gpio_pull_up(g_pins.pard_n);
    gpio_pull_up(g_pins.pawr_n);

    // The physical ROM is rated for 100 ns. Round upward, then retain two clocks of margin.
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    g_rom_access_cycles = ((sys_hz + 9999999u) / 10000000u) + 2;
    gpio_put(g_pins.bus_oe_n, BUS_ENABLE);
}

void snes_bus_start(SuperFx& fx) {
    g_fx = &fx;
    snes_pio_start(g_pins, fx);
    g_started = true;
}

void __not_in_flash_func(snes_irq_write)(void* context, bool asserted) {
    (void)context;
    gpio_put(g_pins.irq_n, asserted ? 0 : 1);
}

uint8_t snes_rom_read(void* context, uint32_t address) {
    (void)context;

    // FX-side code uses the cartridge's separate QSPI FX ROM; the parallel flash
    // belongs to the 65816 path and simultaneous ROM access is explicitly supported.
    // This guard prevents a future callback regression from accidentally reintroducing
    // physical-bus stealing in FX3 mode.
    if (g_fx && g_fx->config().chip == FxChip::FX3)
        return 0xFF;

    // Legacy GSU1/2 only.
    // FIXME: Prove or redesign the legacy GSU ROM-bus grant.
    // Core 0 pauses PIO after observing an idle bus, but the SNES never acknowledges that grant.
    // A new cartridge cycle could therefore begin after isolation has already started.
    g_bus_request.store(true, std::memory_order_release);
    while (!g_bus_granted.load(std::memory_order_acquire)) tight_loop_contents();

    gpio_put(g_pins.rom_oe_n, ROM_DISABLE);

    // FIXME: Verify the mapping from logical GSU ROM addresses to the physical parallel-flash pins.
    // This path drives the 24-bit address directly, so document any required Super FX ROM mirroring
    // or cartridge-side remap and apply it here if the physical image is not one-to-one.
    gpio_put_masked64(SNES_ADDR_MASK, snes_address_output_value(address));
    gpio_set_dir_masked64(SNES_ADDR_MASK, SNES_ADDR_MASK);
    gpio_put(g_pins.rom_oe_n, ROM_ENABLE);

    busy_wait_at_least_cycles(g_rom_access_cycles);
    const uint8_t data = snes_read_data(gpio_get_all64());

    gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
    gpio_set_dir_masked64(SNES_ADDR_MASK, 0);

    g_bus_request.store(false, std::memory_order_release);
    while (g_bus_granted.load(std::memory_order_acquire)) tight_loop_contents();
    return data;
}

void snes_bus_service() {
    if (!g_started) return;

    if (g_bus_granted.load(std::memory_order_acquire)) {
        // FIXME: Prevent or preserve SNES cycles that begin while the legacy ROM grant is active.
        // PIO and BUS_OE are already isolated at this point, so waiting for a new cycle to finish can
        // silently miss it. Prove that no SNES cycle can begin during the grant, or add arbitration
        // that captures and services the transaction.
        // NOTE: /RESET is still latched while PIO is paused, but software applies it only after
        // the in-flight physical ROM access releases this grant.
        if (g_bus_request.load(std::memory_order_acquire) || !snes_bus_idle()) return;

        gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
        gpio_set_dir_masked64(SNES_ADDR_MASK, 0);
        snes_pio_resume();
        g_bus_granted.store(false, std::memory_order_release);
        return;
    }

    if (snes_pio_reset_pending() && !g_bus_request.load(std::memory_order_acquire) && fx_sync_reset()) {
        // Do not acknowledge /RESET until core 1 has accepted it; a full queue must retry next service pass.
        snes_irq_write(nullptr, false);
        snes_pio_clear_reset();
        snes_pio_sync_rom_ownership();
    }

    snes_pio_sync_rom_ownership();
    if (!g_bus_request.load(std::memory_order_acquire) || !snes_bus_idle()) return;

    snes_pio_pause();

    // Re-sample after PIO has been stopped. This closes the obvious check-then-pause
    // window where a SNES cycle begins while snes_pio_pause() is executing. It does
    // not eliminate the later no-handshake window.
    if (!snes_bus_idle()) {
        snes_pio_resume();
        return;
    }

    g_bus_granted.store(true, std::memory_order_release);
}
