/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "qspi_save.h"

#include "snes_bus.h"
#include "snes_pio.h"
#include "storage/fx3_save_journal.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#include <cstring>

#if PICO_FLASH_ASSUME_CORE0_SAFE || PICO_FLASH_ASSUME_CORE1_SAFE
#error "QSPI saves require SDK lockout on both cores"
#endif

namespace {
std::atomic<uint8_t>* g_ram = nullptr;
alignas(4) uint8_t g_snapshot[fx3_save::PAYLOAD_SIZE];
uint32_t g_saved_crc = 0;
bool g_last_usb = false;
bool g_last_reset = false;
bool g_safe = false;
std::atomic<bool> g_ok {true};
std::atomic_flag g_saving = ATOMIC_FLAG_INIT;
bool g_have_save = false;

bool save_range(uint32_t offset, uint32_t size, uint32_t alignment) {
    return g_safe && offset >= fx3_qspi::SAVE_OFFSET &&
        offset <= fx3_qspi::SAVE_OFFSET + fx3_qspi::SAVE_SIZE &&
        size <= fx3_qspi::SAVE_OFFSET + fx3_qspi::SAVE_SIZE - offset &&
        (offset % alignment) == 0 && (size % alignment) == 0;
}

bool __not_in_flash_func(erase)(void*, uint32_t offset, uint32_t size) {
    if (!save_range(offset, size, FLASH_SECTOR_SIZE))
        return false;
    flash_range_erase(offset, size);
    const auto* bytes = reinterpret_cast<const uint8_t*>(XIP_BASE + offset);
    for (uint32_t i = 0; i < size; ++i) {
        if (bytes[i] != 0xFF)
            return false;
    }
    return true;
}

bool __not_in_flash_func(program)(void*, uint32_t offset, const uint8_t* data, uint32_t size) {
    if (!data || !save_range(offset, size, FLASH_PAGE_SIZE))
        return false;
    // The SDK restores XIP before returning; all input pages reside in SRAM.
    flash_range_program(offset, data, size);
    return std::memcmp(reinterpret_cast<const void*>(XIP_BASE + offset), data, size) == 0;
}

fx3_save::QspiFlash flash() {
    return {nullptr, reinterpret_cast<const uint8_t*>(XIP_BASE), fx3_qspi::FLASH_SIZE,
            erase, program};
}

void save_snapshot(void*) {
    // flash_safe_execute parks the OTHER core and disables local interrupts
    // through snapshot capture, commit and verification. Both cores register
    // SDK lockout victims. Only SDK SRAM/ROM routines run while XIP is disabled;
    // flash_range_* restores XIP before returning to journal code or verification.
    // PIO/DMA must not access QSPI or mutate g_ram independently during this call.
    g_safe = true;
    for (uint32_t i = 0; i < sizeof(g_snapshot); ++i)
        g_snapshot[i] = g_ram[i].load(std::memory_order_relaxed);
    const uint32_t crc = fx3_save::crc32(g_snapshot, sizeof(g_snapshot));
    if (!g_have_save || crc != g_saved_crc) {
        g_ok = fx3_save::append(flash(), g_snapshot, sizeof(g_snapshot));
        if (g_ok) {
            g_saved_crc = crc;
            g_have_save = true;
        }
    }
    g_safe = false;
}
}

void qspi_save_init(std::atomic<uint8_t>* ram) {
    g_ram = ram;
    g_have_save = fx3_save::restore(flash(), g_snapshot, sizeof(g_snapshot));
    if (g_have_save) {
        for (uint32_t i = 0; i < sizeof(g_snapshot); ++i)
            ram[i].store(g_snapshot[i], std::memory_order_relaxed);
    } else {
        for (uint32_t i = 0; i < sizeof(g_snapshot); ++i)
            g_snapshot[i] = ram[i].load(std::memory_order_relaxed);
    }
    g_saved_crc = fx3_save::crc32(g_snapshot, sizeof(g_snapshot));
    g_last_usb = gpio_get(SNES_PRES_N_PIN);
    g_last_reset = false;
    g_ok = true;
}

bool qspi_save_now(void*) {
    // Reset/disconnect fallback runs on core 0; the instruction runs on core 1.
    // Never wait for the other writer while it may be requesting our lockout.
    if (!g_ram || g_saving.test_and_set(std::memory_order_acquire))
        return false;
    g_ok = true;
    if (flash_safe_execute(save_snapshot, nullptr, 1000) != PICO_OK)
        g_ok = false;
    const bool ok = g_ok.load();
    g_saving.clear(std::memory_order_release);
    return ok;
}

void qspi_save_task() {
    if (!g_ram)
        return;
    const bool usb = snes_bus_usb_mode();
    const bool reset = !usb && !gpio_get(SNES_I_RESET_N_PIN);
    const bool requested = (!g_last_usb && usb) || (!g_last_reset && reset);
    g_last_usb = usb;
    g_last_reset = reset;
    if (!requested)
        return;

    // Reset is already asserted, or the console is disconnected. Extend an
    // existing reset until commit completes; never invent a mid-game reset.
    if (!usb) {
        gpio_put(SNES_O_RESET_N_PIN, 0);
        snes_pio_pause();
    }
    qspi_save_now(nullptr);
    if (!usb) {
        snes_pio_resume();
        gpio_put(SNES_O_RESET_N_PIN, 1);
    }
}

bool qspi_save_last_ok() { return g_ok; }
