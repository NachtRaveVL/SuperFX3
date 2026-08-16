/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "fx_core.h"

enum class Fx3Command : uint16_t {
    ChunkyToPlanarA = 0,
    ChunkyToPlanarB = 1,
    ChunkyToPlanarC = 2,

    ClearA = 3,
    ClearB = 4,
    ClearC = 5,
};

void SuperFx::process_fx3_command() {
    // FX3 repurposes MERGE into six R0-selected commands. Real hardware uses 0-2
    // to finish chunky-to-planar conversion after PLOT. This firmware retains the
    // original GSU pixel cache, which already writes final 8bpp planar data, so the
    // three C2P commands intentionally do nothing. Commands 3-5 remain real clears.

    if (config_.chip != FxChip::FX3) return;

    switch (static_cast<Fx3Command>(state_.r[0])) {
        case Fx3Command::ChunkyToPlanarA:
        case Fx3Command::ChunkyToPlanarB:
        case Fx3Command::ChunkyToPlanarC:
            // PLOT/pixel-cache writeback already produced the final planar bytes.
            break;

        case Fx3Command::ClearA:
            fx3_clear(0, 8);
            break;

        case Fx3Command::ClearB:
            fx3_clear(9, 17);
            break;

        case Fx3Command::ClearC:
            fx3_clear(18, 26);
            break;

        default:
            // Unknown FX3 commands are no-ops.
            break;
    }
}

// MERGE
// GSU1/2: Normal MERGE instruction.
// FX3: MERGE is repurposed as the FX3 command interface.
// FIXME: Confirm how FX3 MERGE should affect the condition flags.
// MesenCE leaves them unchanged while the current Snes9x FX3 implementation clears them.
// We follow MesenCE for now; verify the hardware behavior before locking this down.
void SuperFx::op_merge() {
    if (config_.chip == FxChip::FX3) {
        process_fx3_command();
        return;
    } else {
        // Normal GSU1 / GSU2 MERGE.
        const uint16_t value = (state_.r[7] & 0xFF00) | (state_.r[8] >> 8);

        write_dst(value);

        state_.flags.carry = (value & 0xE0E0) != 0;
        state_.flags.overflow = (value & 0xC0C0) != 0;
        state_.flags.sign = (value & 0x8080) != 0;
        state_.flags.zero = (value & 0xF0F0) != 0;

        reset_prefix();
    }
}
