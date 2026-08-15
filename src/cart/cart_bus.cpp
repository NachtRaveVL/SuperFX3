/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include <atomic>

#include "cart_bus.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/platform.h"
#include "pico/stdlib.h"

static std::atomic<bool> g_bus_request{false};
static std::atomic<bool> g_bus_granted{false};

static uint32_t g_rom_access_cycles = 0;

// -----------------------------------------------------------------------------
// Internal state
// -----------------------------------------------------------------------------
static CartControlPins g_pins{};

static bool g_prev_rd_n = true;
static bool g_prev_wr_n = true;

static bool g_data_driving = false;
static bool g_reset_asserted = false;

// -----------------------------------------------------------------------------
// GPIO helpers
// -----------------------------------------------------------------------------
static inline uint32_t cart_read_address() {
    const uint64_t gpio = gpio_get_all64();
    const uint32_t addr_lo = static_cast<uint32_t>(gpio & 0xFFFFu);
    const uint32_t addr_hi = static_cast<uint32_t>((gpio >> CART_ADDR_HI_BASE) & 0xFFu);

    return addr_lo | (addr_hi << 16);
}

static inline uint8_t cart_read_data() {
    const uint64_t gpio = gpio_get_all64();
    return static_cast<uint8_t>((gpio >> CART_DATA_BASE) & 0xFFu);
}

static inline uint64_t cart_address_output_value(uint32_t address) {
    return static_cast<uint64_t>(address & 0xFFFFu) |
           (static_cast<uint64_t>((address >> 16) & 0xFFu) << CART_ADDR_HI_BASE);
}

static inline void cart_data_input() {
    // Isolate the SNES side while changing direction.
    gpio_put(g_pins.bus_oe_n, BUS_DISABLE);

    // RP2350 listens to D0-D7.
    gpio_set_dir_masked64(CART_DATA_MASK, 0);

    // SNES -> cartridge.
    gpio_put(g_pins.data_dir, DATA_DIR_IN);

    // Reconnect the cartridge bus.
    gpio_put(g_pins.bus_oe_n, BUS_ENABLE);

    g_data_driving = false;
}

static inline void cart_data_output(uint8_t value) {
    // Isolate the SNES side while changing direction.
    gpio_put(g_pins.bus_oe_n, BUS_DISABLE);

    // Load the output latch before enabling the RP2350 data drivers.
    gpio_put_masked64(CART_DATA_MASK, static_cast<uint64_t>(value) << CART_DATA_BASE);
    gpio_set_dir_masked64(CART_DATA_MASK, CART_DATA_MASK);

    // Cartridge -> SNES.
    gpio_put(g_pins.data_dir, DATA_DIR_OUT);

    // Reconnect the cartridge bus.
    gpio_put(g_pins.bus_oe_n, BUS_ENABLE);

    g_data_driving = true;
}

// -----------------------------------------------------------------------------
// SuperFX register decode
// -----------------------------------------------------------------------------
static inline bool cart_is_gsu_bank(uint8_t bank) { return bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF); }

static inline bool cart_is_gsu_register(const SuperFx& fx, uint32_t address) {
    const uint8_t bank = static_cast<uint8_t>(address >> 16);

    if (!cart_is_gsu_bank(bank)) {
        return false;
    }

    const uint16_t addr = static_cast<uint16_t>(address);

    if (fx.config().chip == FxChip::FX3) {
        return addr >= 0x7000 && addr <= 0x7FFF;
    }

    return addr >= 0x3000 && addr <= 0x3FFF;
}

static inline bool cart_gsu_ram_offset(const SuperFx& fx, uint32_t address, uint32_t& offset) {
    const uint8_t bank = static_cast<uint8_t>(address >> 16);
    const uint16_t addr = static_cast<uint16_t>(address);

    if (bank == 0x70 || bank == 0x71) {
        offset = (static_cast<uint32_t>(bank - 0x70) << 16) | addr;
        return true;
    }

    if (fx.config().chip == FxChip::FX3) return false;

    if ((bank <= 0x3E || (bank >= 0x80 && bank <= 0xBE)) && addr >= 0x6000 && addr <= 0x7FFF) {
        offset = addr - 0x6000;
        return true;
    }

    if (bank == 0xF0 || bank == 0xF1) {
        offset = (static_cast<uint32_t>(bank - 0xF0) << 16) | addr;
        return true;
    }

    return false;
}

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------
void cart_bus_init(const CartControlPins& pins) {
    g_pins = pins;

    g_bus_request.store(false, std::memory_order_relaxed);
    g_bus_granted.store(false, std::memory_order_relaxed);

    // PIO GPIO windows.
    pio_set_gpio_base(pio0, 0);
    pio_set_gpio_base(pio1, 16);

    // A0-A15: GPIO 0-15.
    for (uint8_t pin = CART_ADDR_LO_BASE; pin < CART_ADDR_LO_BASE + 16; pin++) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    // Control bus: GPIO 16-31.
    // Start everything as an input; control outputs are configured below.
    for (uint8_t pin = CART_CTRL_BASE; pin < CART_CTRL_BASE + 16; pin++) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    // A16-A23: GPIO 32-39.
    for (uint8_t pin = CART_ADDR_HI_BASE; pin < CART_ADDR_HI_BASE + 8; pin++) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    // D0-D7: GPIO 40-47.
    for (uint8_t pin = CART_DATA_BASE; pin < CART_DATA_BASE + 8; pin++) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
        gpio_set_dir(pin, GPIO_IN);
    }

    // Control outputs

    // Keep the SNES side isolated until every output has a safe value.
    gpio_put(g_pins.bus_oe_n, BUS_DISABLE);
    gpio_set_dir(g_pins.bus_oe_n, GPIO_OUT);
    // Default data direction: SNES -> cartridge.
    gpio_put(g_pins.data_dir, DATA_DIR_IN);
    gpio_set_dir(g_pins.data_dir, GPIO_OUT);
    // External ROM output disabled by default.
    gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
    gpio_set_dir(g_pins.rom_oe_n, GPIO_OUT);
    // /IRQ inactive by default.
    gpio_put(g_pins.irq_n, 1);
    gpio_set_dir(g_pins.irq_n, GPIO_OUT);

    // Pull-ups on active-low SNES inputs
    gpio_pull_up(g_pins.rd_n);
    gpio_pull_up(g_pins.wr_n);
    gpio_pull_up(g_pins.cart_n);
    gpio_pull_up(g_pins.reset_n);
    gpio_pull_up(g_pins.wramsel_n);
    gpio_pull_up(g_pins.pard_n);
    gpio_pull_up(g_pins.pawr_n);

    // Physical ROM timing
    const uint32_t sys_hz = clock_get_hz(clk_sys);
    // 100 ns = 1 / 10,000,000 second.
    // Round up and add two system-clock cycles of margin.
    g_rom_access_cycles = ((sys_hz + 9999999u) / 10000000u) + 2;

    // Normal idle state: SNES bus connected, RP2350 listening.
    gpio_put(g_pins.bus_oe_n, BUS_ENABLE);

    g_prev_rd_n = gpio_get(g_pins.rd_n);
    g_prev_wr_n = gpio_get(g_pins.wr_n);
    g_data_driving = false;
    g_reset_asserted = false;
}

// -----------------------------------------------------------------------------
// /IRQ
// -----------------------------------------------------------------------------
void cart_irq_write(void* context, bool asserted) {
    (void)context;

    gpio_put(g_pins.irq_n, asserted ? 0 : 1);
}

// -----------------------------------------------------------------------------
// GSU-side physical ROM read
// -----------------------------------------------------------------------------
uint8_t cart_rom_read(void* context, uint32_t address) {
    (void)context;

    // Ask core 0 for exclusive ownership of the physical cartridge bus.
    g_bus_request.store(true, std::memory_order_release);

    while (!g_bus_granted.load(std::memory_order_acquire)) tight_loop_contents();

    // Core 0 has disabled /BUS_OE. The SNES is isolated.
    gpio_put(g_pins.rom_oe_n, ROM_DISABLE);

    // ROM drives D0-D7 during this access.
    gpio_set_dir_masked64(CART_DATA_MASK, 0);
    // Put the requested address in the output latch before driving A0-A23.
    gpio_put_masked64(CART_ADDR_MASK, cart_address_output_value(address));
    gpio_set_dir_masked64(CART_ADDR_MASK, CART_ADDR_MASK);

    // Start physical ROM access.
    gpio_put(g_pins.rom_oe_n, ROM_ENABLE);

    // Wait worst case then capture data.
    busy_wait_at_least_cycles(g_rom_access_cycles);
    const uint8_t data = cart_read_data();

    // End the ROM access before releasing the address bus.
    gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
    gpio_set_dir_masked64(CART_ADDR_MASK, 0);
    gpio_set_dir_masked64(CART_DATA_MASK, 0);

    // Tell core 0 that it may reconnect the SNES side.
    g_bus_request.store(false, std::memory_order_release);

    while (g_bus_granted.load(std::memory_order_acquire)) tight_loop_contents();

    return data;
}

// -----------------------------------------------------------------------------
// SNES bus service (polling-based)
// Handles:
//   - SuperFX register reads/writes
//   - SuperFX RAM reads/writes
//   - External ROM decode and /ROM_OE
//   - Data-bus direction
//   - GSU requests for exclusive cartridge-bus ownership
// -----------------------------------------------------------------------------
void cart_bus_service(SuperFx& fx) {
    const bool reset_n = gpio_get(g_pins.reset_n);
    const bool rd_n = gpio_get(g_pins.rd_n);
    const bool wr_n = gpio_get(g_pins.wr_n);
    const bool cart_n = gpio_get(g_pins.cart_n);
    const bool pard_n = gpio_get(g_pins.pard_n);
    const bool pawr_n = gpio_get(g_pins.pawr_n);

    if (g_bus_granted.load(std::memory_order_acquire)) {
        if (g_bus_request.load(std::memory_order_acquire)) return;

        gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
        gpio_set_dir_masked64(CART_DATA_MASK, 0);
        gpio_put(g_pins.data_dir, DATA_DIR_IN);
        gpio_put(g_pins.bus_oe_n, BUS_ENABLE);

        g_data_driving = false;
        g_bus_granted.store(false, std::memory_order_release);

        g_prev_rd_n = rd_n;
        g_prev_wr_n = wr_n;
        return;
    }

    if (g_bus_request.load(std::memory_order_acquire)) {
        const bool bus_idle = rd_n && wr_n && cart_n && pard_n && pawr_n;

        if (bus_idle) {
            gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
            gpio_set_dir_masked64(CART_DATA_MASK, 0);
            gpio_put(g_pins.data_dir, DATA_DIR_IN);
            gpio_put(g_pins.bus_oe_n, BUS_DISABLE);

            g_data_driving = false;
            g_bus_granted.store(true, std::memory_order_release);
        }

        return;
    }

    if (!reset_n) {
        gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
        cart_data_input();

        if (!g_reset_asserted) {
            fx.reset();
            g_reset_asserted = true;
        }

        g_prev_rd_n = rd_n;
        g_prev_wr_n = wr_n;
        return;
    }

    g_reset_asserted = false;

    if (rd_n && !g_prev_rd_n) {
        gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
        cart_data_input();
    }

    if (rd_n && wr_n) {
        g_prev_rd_n = true;
        g_prev_wr_n = true;
        return;
    }

    const uint32_t address = cart_read_address();
    const uint16_t addr = static_cast<uint16_t>(address);
    const bool gsu_register = cart_is_gsu_register(fx, address);
    uint32_t ram_offset = 0;
    const bool gsu_ram = cart_gsu_ram_offset(fx, address, ram_offset);

    if (!wr_n && g_prev_wr_n) {
        gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
        cart_data_input();

        const uint8_t data = cart_read_data();

        if (gsu_register) {
            fx.cpu_write(addr, data);
        } else if (gsu_ram) {
            fx.cpu_ram_write(ram_offset, data);
        }
    }

    if (!rd_n && g_prev_rd_n) {
        if (gsu_register) {
            gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
            cart_data_output(fx.cpu_read(addr));
        } else if (gsu_ram) {
            gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
            cart_data_output(fx.cpu_ram_read(ram_offset));
        } else if (!cart_n) {
            if (!fx.rom_access_allowed()) {
                gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
                cart_data_output(fx.blocked_rom_value(address));
            } else {
                gpio_put(g_pins.data_dir, DATA_DIR_OUT);
                gpio_set_dir_masked64(CART_DATA_MASK, 0);
                g_data_driving = false;

                gpio_put(g_pins.rom_oe_n, ROM_ENABLE);
            }
        } else {
            gpio_put(g_pins.rom_oe_n, ROM_DISABLE);
            cart_data_input();
        }
    }

    g_prev_rd_n = rd_n;
    g_prev_wr_n = wr_n;
}
