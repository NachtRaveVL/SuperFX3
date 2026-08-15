/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"

const FxConfig fx1_config {
    FxChip::GSU1, FxTiming::Accurate, 0x5F
};

const FxConfig fx2_config {
    FxChip::GSU2, FxTiming::Accurate, 0x5F
};

const FxConfig fx3_config {
    FxChip::FX3, FxTiming::Unlimited, 0x6F
};

// -------------------------------------------------------------
// Cycle-accurate GSU1 / GSU2
// snes_master_clock is a free-running 32-bit hardware counter.
// -------------------------------------------------------------
void SuperFx::run_accurate(uint32_t snes_master_clock) {
    if (!timing_initialized_) {
        last_master_clock_ = snes_master_clock;
        target_cycles_ = state_.cycles;
        timing_initialized_ = true;
        return;
    }

    const uint32_t elapsed = snes_master_clock - last_master_clock_;
    last_master_clock_ = snes_master_clock;
    target_cycles_ += elapsed;

    // Run until the real hardware timeline catches us.
    while (!stopped_ && state_.cycles < target_cycles_) {
        execute();
    }

    // If halted/stalled, time still passes.
    // ROM/RAM operations continue advancing.
    if (state_.cycles < target_cycles_) {
        const uint64_t remaining = target_cycles_ - state_.cycles;
        step(static_cast<uint32_t>(remaining));
    }
}

void SuperFx::run_unlimited(uint32_t instruction_budget) {
    while (!stopped_ && instruction_budget--) {
        execute();
    }
}

void SuperFx::run(uint32_t snes_master_clock, uint32_t unlimited_budget) {
    if (config_.timing == FxTiming::Accurate) {
        run_accurate(snes_master_clock);
    } else {
        run_unlimited(unlimited_budget);
    }
}
