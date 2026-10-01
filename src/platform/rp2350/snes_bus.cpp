/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include <atomic>

#include "snes_bus.h"
#include "snes_bus_layout.h"
#include "snes_pio.h"
#include "fx_sync.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico.h"
#include "pico/stdlib.h"
#include "pico/sync.h"

#ifndef SNES_FX3
#error "This firmware requires PICO_BOARD=snes_fx3"
#endif

static std::atomic<bool> g_bus_request {false};
static std::atomic<bool> g_bus_granted {false};
static SuperFx* g_fx = nullptr;
static uint32_t g_rom_access_cycles = 0;
static uint32_t g_rom_address_setup_cycles = 0;
static uint32_t g_rom_address_hold_cycles = 0;
static bool g_started = false;
static bool g_console_present = false;
static critical_section_t g_irq_gate;
static bool g_core_irq = false;
static bool g_busy_irq = false;

static void snes_local_control(uint8_t word) {
    gpio_put_masked64(
        SNES_LOCAL_CONTROL_MASK,
        static_cast<uint64_t>(word) << SNES_LOCAL_CONTROL_BASE
    );
    gpio_set_dir_masked64(SNES_LOCAL_CONTROL_MASK, SNES_LOCAL_CONTROL_MASK);
    for (uint pin = SNES_LOCAL_CONTROL_BASE;
         pin < SNES_LOCAL_CONTROL_BASE + SNES_LOCAL_CONTROL_COUNT; ++pin) {
        gpio_set_function(pin, GPIO_FUNC_SIO);
    }
}

static bool snes_console_detected() {
    return !gpio_get(SNES_PRES_N_PIN);
}

// /I_RD and /I_WR are translated inputs only. The one exception in this
// group is /I_RST: with /C_OE disabled it is locally driven so it cannot float.
static void snes_take_local_reset() {
    gpio_set_dir_masked64(SNES_I_CONTROL_MASK, 0);
    gpio_put(SNES_I_RESET_N_PIN, 0);
    gpio_set_dir(SNES_I_RESET_N_PIN, GPIO_OUT);
}

static void snes_release_local_reset_to_console() {
    gpio_set_dir(SNES_I_RESET_N_PIN, GPIO_IN);
    gpio_set_dir_masked64(SNES_I_CONTROL_MASK, 0);
}

static void snes_enter_standalone() {
    if (g_started)
        snes_pio_stop();

    // Disable every translator before taking local ownership of /I_RST.
    // GPIO1/2 remain the only /RD and /WR outputs to the physical ROM.
    snes_local_control(SNES_CONTROL_STANDALONE);
    snes_take_local_reset();
    if (g_started) {
        while (!fx_sync_reset())
            tight_loop_contents();
    }
    gpio_put(SNES_I_RESET_N_PIN, 1);

    g_bus_request.store(false, std::memory_order_release);
    g_bus_granted.store(false, std::memory_order_release);
    g_console_present = false;
}

static void snes_enter_console() {
    // /C_OE is still disabled here. Release /I_RST and all other translated
    // inputs before snes_pio_resume() enables /C_OE in its idle control word.
    snes_local_control(SNES_CONTROL_STANDALONE);
    snes_release_local_reset_to_console();
    snes_pio_resume();
    g_console_present = true;
}

// Returns true only when the translated console bus is between transactions.
static bool snes_bus_idle() {
    return gpio_get(SNES_I_RD_N_PIN) && gpio_get(SNES_I_WR_N_PIN) &&
           gpio_get(SNES_I_CART_N_PIN);
}

void snes_bus_init() {
    critical_section_init(&g_irq_gate);
    g_core_irq = false;
    g_busy_irq = false;
    g_bus_request.store(false, std::memory_order_relaxed);
    g_bus_granted.store(false, std::memory_order_relaxed);
    g_started = false;
    g_console_present = false;

    gpio_init(SNES_PRES_N_PIN);
    gpio_set_dir(SNES_PRES_N_PIN, GPIO_IN);
    gpio_pull_up(SNES_PRES_N_PIN);

    for (uint pin = SNES_ADDR_RAW_BASE;
         pin < SNES_ADDR_RAW_BASE + SNES_ADDR_RAW_COUNT; ++pin) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_IN);
    }
    for (uint pin = SNES_DATA_RAW_BASE;
         pin < SNES_DATA_RAW_BASE + SNES_DATA_RAW_COUNT; ++pin) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_IN);
    }
    for (uint pin = SNES_I_IRQ_N_PIN; pin <= SNES_I_CLK_PIN; ++pin) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_up(pin);
    }

    // Establish disabled ROM/transceiver strobes before any bus output is live.
    for (uint pin = SNES_LOCAL_CONTROL_BASE;
         pin < SNES_LOCAL_CONTROL_BASE + SNES_LOCAL_CONTROL_COUNT; ++pin) {
        gpio_init(pin);
    }
    snes_local_control(SNES_CONTROL_STANDALONE);

    gpio_init(SNES_O_IRQ_N_PIN);
    gpio_put(SNES_O_IRQ_N_PIN, 1);
    gpio_set_dir(SNES_O_IRQ_N_PIN, GPIO_OUT);
    gpio_init(SNES_O_RESET_N_PIN);
    gpio_put(SNES_O_RESET_N_PIN, 1);
    gpio_set_dir(SNES_O_RESET_N_PIN, GPIO_OUT);

    // /C_OE is disabled, so explicitly drive /I_RST inactive rather than
    // leaving the RP2350-side control net floating. /I_RD and /I_WR stay inputs.
    snes_take_local_reset();
    gpio_put(SNES_I_RESET_N_PIN, 1);

    // The single 128-Mbit-capable parallel ROM is rated for 100 ns.
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    g_rom_access_cycles = ((sys_hz + 9999999u) / 10000000u) + 2;
    g_rom_address_setup_cycles = ((sys_hz + 49999999u) / 50000000u) + 1;
    g_rom_address_hold_cycles = ((sys_hz + 99999999u) / 100000000u) + 1;
}

void snes_bus_start(SuperFx& fx) {
    g_fx = &fx;
    snes_pio_start(fx);
    g_started = true;
    if (snes_console_detected())
        snes_enter_console();
    else
        snes_enter_standalone();
}

bool snes_bus_usb_mode() {
    return g_started && !g_console_present && !snes_console_detected();
}

bool snes_bus_local_mode() { return !g_console_present; }

void __not_in_flash_func(snes_irq_write)(void* context, bool asserted) {
    (void)context;
    critical_section_enter_blocking(&g_irq_gate);
    g_core_irq = asserted;
    gpio_put(SNES_O_IRQ_N_PIN, (g_core_irq || g_busy_irq) ? 0 : 1);
    critical_section_exit(&g_irq_gate);
}

void snes_busy_irq_write(void*, bool asserted) {
    critical_section_enter_blocking(&g_irq_gate);
    g_busy_irq = asserted;
    gpio_put(SNES_O_IRQ_N_PIN, (g_core_irq || g_busy_irq) ? 0 : 1);
    critical_section_exit(&g_irq_gate);
}

uint8_t snes_rom_read(void* context, uint32_t address) {
    (void)context;

    // FX3 executes private code from QSPI and must never steal the parallel bus.
    if (!g_console_present || (g_fx && g_fx->config().chip == FxChip::FX3))
        return 0xFF;

    g_bus_request.store(true, std::memory_order_release);
    while (!g_bus_granted.load(std::memory_order_acquire))
        tight_loop_contents();

    gpio_put_masked64(SNES_ADDR_MASK, snes_address_to_gpio(address));
    gpio_set_dir_masked64(SNES_ADDR_MASK, SNES_ADDR_MASK);

    busy_wait_at_least_cycles(g_rom_address_setup_cycles);
    snes_local_control(SNES_CONTROL_ROM_READ);
    busy_wait_at_least_cycles(g_rom_access_cycles);
    const uint8_t data = snes_data_from_gpio(gpio_get_all64());

    snes_local_control(SNES_CONTROL_BUS_ISOLATED);
    busy_wait_at_least_cycles(g_rom_address_hold_cycles);
    gpio_set_dir_masked64(SNES_ADDR_MASK, 0);

    g_bus_request.store(false, std::memory_order_release);
    while (g_bus_granted.load(std::memory_order_acquire))
        tight_loop_contents();
    return data;
}

void snes_bus_service() {
    if (!g_started)
        return;

    const bool detected = snes_console_detected();
    if (detected != g_console_present) {
        if (detected)
            snes_enter_console();
        else
            snes_enter_standalone();
        return;
    }
    if (!g_console_present)
        return;

    if (g_bus_granted.load(std::memory_order_acquire)) {
        if (g_bus_request.load(std::memory_order_acquire) || !snes_bus_idle())
            return;

        gpio_set_dir_masked64(SNES_ADDR_MASK, 0);
        snes_pio_resume();
        g_bus_granted.store(false, std::memory_order_release);
        return;
    }

    if (snes_pio_reset_pending() &&
        !g_bus_request.load(std::memory_order_acquire) && snes_pio_service_reset()) {
        snes_irq_write(nullptr, false);
        snes_pio_sync_rom_ownership();
    }

    snes_pio_sync_rom_ownership();
    if (!g_bus_request.load(std::memory_order_acquire) || !snes_bus_idle())
        return;

    snes_pio_pause();
    if (!snes_bus_idle()) {
        snes_pio_resume();
        return;
    }
    g_bus_granted.store(true, std::memory_order_release);
}
