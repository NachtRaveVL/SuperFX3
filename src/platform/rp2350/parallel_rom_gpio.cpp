/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "parallel_rom_gpio.h"

#include "snes_bus.h"
#include "snes_bus_layout.h"

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/time.h"

namespace {
constexpr uint32_t ADDRESS_SETUP_CYCLES = 4;
constexpr uint32_t WRITE_PULSE_CYCLES = 8;
constexpr uint32_t READ_ACCESS_CYCLES = 18;

void local_control(uint8_t word) {
    gpio_put_masked64(SNES_LOCAL_CONTROL_MASK,
                      static_cast<uint64_t>(word) << SNES_LOCAL_CONTROL_BASE);
    gpio_set_dir_masked64(SNES_LOCAL_CONTROL_MASK, SNES_LOCAL_CONTROL_MASK);
    for (uint pin = SNES_LOCAL_CONTROL_BASE;
         pin < SNES_LOCAL_CONTROL_BASE + SNES_LOCAL_CONTROL_COUNT; ++pin)
        gpio_set_function(pin, GPIO_FUNC_SIO);
}

void set_address(uint32_t address) {
    gpio_put_masked64(SNES_ADDR_MASK, snes_address_to_gpio(address));
    gpio_set_dir_masked64(SNES_ADDR_MASK, SNES_ADDR_MASK);
}

uint8_t read_byte(void*, uint32_t address) {
    // A read-only boot probe is also safe before the console translators connect.
    if (!snes_bus_local_mode())
        return 0xFF;
    set_address(address);
    gpio_set_dir_masked64(SNES_DATA_MASK, 0);
    busy_wait_at_least_cycles(ADDRESS_SETUP_CYCLES);
    local_control(SNES_CONTROL_ROM_VERIFY);
    busy_wait_at_least_cycles(READ_ACCESS_CYCLES);
    const uint8_t value = snes_data_from_gpio(gpio_get_all64());
    local_control(SNES_CONTROL_STANDALONE);
    gpio_set_dir_masked64(SNES_ADDR_MASK, 0);
    return value;
}

void write_byte(void*, uint32_t address, uint8_t data) {
    // Abort may reset the NOR after SNES presence changes, but only while
    // translators remain isolated. Ordinary programming still requires USB mode.
    const bool isolated_reset = snes_bus_local_mode() && address == 0 && data == 0xF0;
    if (!snes_bus_usb_mode() && !isolated_reset)
        return;
    set_address(address);
    gpio_put_masked64(SNES_DATA_MASK, snes_data_to_gpio(data));
    gpio_set_dir_masked64(SNES_DATA_MASK, SNES_DATA_MASK);
    local_control(SNES_CONTROL_ROM_WRITE_SETUP);
    busy_wait_at_least_cycles(ADDRESS_SETUP_CYCLES);
    local_control(SNES_CONTROL_ROM_WRITE);
    busy_wait_at_least_cycles(WRITE_PULSE_CYCLES);
    local_control(SNES_CONTROL_ROM_WRITE_SETUP);
    busy_wait_at_least_cycles(ADDRESS_SETUP_CYCLES);
    local_control(SNES_CONTROL_STANDALONE);
    gpio_set_dir_masked64(SNES_DATA_MASK, 0);
    gpio_set_dir_masked64(SNES_ADDR_MASK, 0);
}

uint64_t time_us(void*) {
    return time_us_64();
}
}

ParallelRomBus parallel_rom_gpio_bus() {
    return {nullptr, read_byte, write_byte, time_us, nullptr};
}
