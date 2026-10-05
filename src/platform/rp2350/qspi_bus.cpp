/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "qspi_bus.h"

#include <atomic>

#include "fx_sync.h"
#include "snes_pio.h"
#include "hardware/gpio.h"
#include "pico.h"
#include "pico/stdlib.h"

namespace {
std::atomic_flag g_owner = ATOMIC_FLAG_INIT;
#if SUPERFX3_AUDIO_SD
enum class AudioPhase : uint8_t { Idle, Requested, Preparing, Active, Ending };
std::atomic<AudioPhase> g_audio_phase {AudioPhase::Idle};
std::atomic<bool> g_audio_cancelled {false};

void __no_inline_not_in_flash_func(qspi_bus_core0_wait)() {
    g_audio_phase.store(AudioPhase::Active, std::memory_order_release);
    while (g_audio_phase.load(std::memory_order_acquire) == AudioPhase::Active) {
        if (gpio_get(SNES_PRES_PIN) != SNES_PRES_ACTIVE_LEVEL ||
            !gpio_get(SNES_I_RESET_N_PIN))
            g_audio_cancelled.store(true, std::memory_order_release);
        __compiler_memory_barrier();
    }
}
#endif
}

bool qspi_bus_try_acquire() {
    return !g_owner.test_and_set(std::memory_order_acquire);
}

void qspi_bus_release() {
    g_owner.clear(std::memory_order_release);
}

void __not_in_flash_func(qspi_bus_audio_cancel)() {
#if SUPERFX3_AUDIO_SD
    g_audio_cancelled.store(true, std::memory_order_release);
#endif
}

bool __not_in_flash_func(qspi_bus_audio_cancelled)() {
#if SUPERFX3_AUDIO_SD
    return g_audio_cancelled.load(std::memory_order_acquire);
#else
    return true;
#endif
}

bool qspi_bus_audio_begin() {
#if SUPERFX3_AUDIO_SD
    if (!qspi_bus_try_acquire())
        return false;
    g_audio_cancelled.store(false, std::memory_order_relaxed);
    g_audio_phase.store(AudioPhase::Requested, std::memory_order_release);
    const uint32_t start = time_us_32();
    while (true) {
        const AudioPhase phase = g_audio_phase.load(std::memory_order_acquire);
        if (phase == AudioPhase::Active)
            return true;
        if (phase == AudioPhase::Idle) {
            qspi_bus_release();
            return false;
        }
        if (static_cast<uint32_t>(time_us_32() - start) >= 100000u) {
            AudioPhase expected = AudioPhase::Requested;
            if (g_audio_phase.compare_exchange_strong(expected, AudioPhase::Idle,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                qspi_bus_release();
                return false;
            }
            g_audio_cancelled.store(true, std::memory_order_release);
        }
        tight_loop_contents();
    }
#else
    return false;
#endif
}

void qspi_bus_audio_end() {
#if SUPERFX3_AUDIO_SD
    // XIP is restored before freeing the owner or publishing the end of this window.
    qspi_bus_release();
    g_audio_phase.store(AudioPhase::Ending, std::memory_order_release);
    while (g_audio_phase.load(std::memory_order_acquire) != AudioPhase::Idle) {
        // A full write queue must not leave a PIO IRQ waiting for its own core's return.
        fx_sync_sd_drain_writes();
        tight_loop_contents();
    }
    fx_sync_sd_drain_writes();
#endif
}

void qspi_bus_core0_service() {
#if SUPERFX3_AUDIO_SD
    AudioPhase expected = AudioPhase::Requested;
    if (!g_audio_phase.compare_exchange_strong(expected, AudioPhase::Preparing,
            std::memory_order_acq_rel, std::memory_order_acquire))
        return;
    if (!snes_pio_sd_begin()) {
        g_audio_cancelled.store(true, std::memory_order_relaxed);
        g_audio_phase.store(AudioPhase::Idle, std::memory_order_release);
        return;
    }
    qspi_bus_core0_wait();
    snes_pio_sd_end();
    g_audio_phase.store(AudioPhase::Idle, std::memory_order_release);
#endif
}
