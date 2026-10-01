/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#include "fx_core.h"

// Mesen-derived: opcode map follows MesenCE Gsu::Exec(), reorganized by high nibble.
void SuperFx::execute_opcode(uint8_t opcode) {
    // Most GSU opcode families encode the target/operand register directly in
    // the low nibble, so split it once and dispatch primarily by high nibble.
    const uint8_t reg = opcode & 0x0F;
    switch (opcode >> 4) {
        // $00-$0F: STOP / NOP / CACHE / shifts / branches
        case 0x0:
            switch (reg) {
                case 0x0:
                    op_stop();
                    break;

                case 0x1:
                    op_nop();
                    break;

                case 0x2:
                    op_cache();
                    break;

                case 0x3:
                    op_lsr();
                    break;

                case 0x4:
                    op_rol();
                    break;

                case 0x5: // BRA
                    op_branch(true);
                    break;

                case 0x6: // BGE
                    op_branch(state_.flags.sign == state_.flags.overflow);
                    break;

                case 0x7: // BLT
                    op_branch(state_.flags.sign != state_.flags.overflow);
                    break;

                case 0x8: // BNE
                    op_branch(!state_.flags.zero);
                    break;

                case 0x9: // BEQ
                    op_branch(state_.flags.zero);
                    break;

                case 0xA: // BPL
                    op_branch(!state_.flags.sign);
                    break;

                case 0xB: // BMI
                    op_branch(state_.flags.sign);
                    break;

                case 0xC: // BCC
                    op_branch(!state_.flags.carry);
                    break;

                case 0xD: // BCS
                    op_branch(state_.flags.carry);
                    break;

                case 0xE: // BVC
                    op_branch(!state_.flags.overflow);
                    break;

                case 0xF: // BVS
                    op_branch(state_.flags.overflow);
                    break;
            }
            break;

        // $10-$1F: TO Rn / MOVE Rn
        case 0x1:
            op_to(reg);
            break;

        // $20-$2F: WITH Rn
        case 0x2:
            op_with(reg);
            break;

        // $30-$3F: STW/STB / LOOP / ALT1 / ALT2 / ALT3
        case 0x3:
            switch (reg) {
                case 0xC:
                    op_loop();
                    break;

                case 0xD:
                    op_alt1();
                    break;

                case 0xE:
                    op_alt2();
                    break;

                case 0xF:
                    op_alt3();
                    break;

                default:
                    // $30-$3B: STW / STB
                    op_store(reg);
                    break;
            }
            break;

        // $40-$4F: LDW/LDB / PLOT/RPIX / SWAP / COLOR/CMODE / NOT
        case 0x4:
            switch (reg) {
                case 0xC:
                    op_plot_rpix();
                    break;

                case 0xD:
                    op_swap();
                    break;

                case 0xE:
                    op_color_cmode();
                    break;

                case 0xF:
                    op_not();
                    break;

                default:
                    // $40-$4B: LDW / LDB
                    op_load(reg);
                    break;
            }
            break;

        // $50-$5F: ADD / ADC / ADD #n
        case 0x5:
            op_add(reg);
            break;

        // $60-$6F: SUB / SBC / SUB #n / CMP
        case 0x6:
            op_sub_compare(reg);
            break;

        // $70-$7F: MERGE / AND / BIC
        case 0x7:
            if (reg == 0)
                op_merge();
            else
                op_and_bic(reg);
            break;

        // $80-$8F: MULT / UMULT
        case 0x8:
            op_mult(reg);
            break;

        // $90-$9F: SBK / LINK / SEX / ASR / ROR / JMP / LOB /
        //          FMULT/LMULT
        case 0x9:
            switch (reg) {
                case 0x0: // SBK
                    op_sbk();
                    break;

                case 0x1: // LINK #1
                case 0x2: // LINK #2
                case 0x3: // LINK #3
                case 0x4: // LINK #4
                    op_link(reg);
                    break;

                case 0x5: // SEX
                    op_sex();
                    break;

                case 0x6: // ASR
                    op_asr();
                    break;

                case 0x7: // ROR
                    op_ror();
                    break;

                case 0x8:
                case 0x9:
                case 0xA:
                case 0xB:
                case 0xC:
                case 0xD:
                    // $98-$9D: JMP Rn / LJMP Rn
                    op_jmp(reg);
                    break;

                case 0xE: // LOB
                    op_lob();
                    break;

                case 0xF: // FMULT / LMULT
                    op_fmult_lmult();
                    break;
            }
            break;

        // $A0-$AF: IBT / SMS / LMS
        case 0xA:
            op_ibt_sms_lms(reg);
            break;

        // $B0-$BF: FROM Rn / MOVES Rn
        case 0xB:
            op_from(reg);
            break;

        // $C0-$CF: HIB / OR / XOR
        case 0xC:
            if (reg == 0)
                op_hib();
            else
                op_or_xor(reg);
            break;

        // $D0-$DF: INC Rn / GETC / RAMB / ROMB
        case 0xD:
            if (reg == 0xF)
                op_getc_ramb_romb();
            else
                op_inc(reg);
            break;

        // $E0-$EF: DEC Rn / GETB
        case 0xE:
            if (reg == 0xF)
                op_getb();
            else
                op_dec(reg);
            break;

        // $F0-$FF: IWT / LM / SM
        case 0xF:
            op_iwt_lm_sm(reg);
            break;
    }
}

void SuperFx::unimplemented(uint8_t opcode) {
    (void)opcode;
    state_.flags.running = false;
    update_running_state();
}
