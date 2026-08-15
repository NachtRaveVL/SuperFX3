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

// -----------------------------------------------------------------------------
// FX3 command dispatcher
// -----------------------------------------------------------------------------
void SuperFx::process_fx3_command() {
    if (config_.chip != FxChip::FX3) return;

    switch (static_cast<Fx3Command>(state_.r[0])) {
        case Fx3Command::ChunkyToPlanarA:
            fx3_chunky_to_planar(0);
            break;

        case Fx3Command::ChunkyToPlanarB:
            fx3_chunky_to_planar(1);
            break;

        case Fx3Command::ChunkyToPlanarC:
            fx3_chunky_to_planar(2);
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

// -----------------------------------------------------------------------------
// MERGE
// GSU1/2:
//     Normal MERGE instruction.
// FX3:
//     MERGE is repurposed as the FX3 command interface.
// -----------------------------------------------------------------------------
void SuperFx::op_merge() {
    if (config_.chip == FxChip::FX3) {
        process_fx3_command();
        return;
    }

    // Normal GSU1 / GSU2 MERGE.
    const uint16_t value = (state_.r[7] & 0xFF00) | (state_.r[8] >> 8);

    write_dst(value);

    state_.flags.carry = (value & 0xE0E0) != 0;
    state_.flags.overflow = (value & 0xC0C0) != 0;
    state_.flags.sign = (value & 0x8080) != 0;
    state_.flags.zero = (value & 0xF0F0) != 0;

    reset_prefix();
}
