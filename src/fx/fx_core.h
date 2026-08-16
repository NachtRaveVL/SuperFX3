/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "fx_state.h"

struct FxBackend {
    void* context;                                           ///< Opaque context passed to each backend callback.

    uint8_t (*rom_read)(void* context, uint32_t address);    ///< Callback used to read a byte from the ROM backend.
    uint8_t (*ram_read)(void* context, uint32_t address);    ///< Callback used to read a byte from CPU-visible FX SRAM.

    void (*ram_write)(void* context, uint32_t address, uint8_t value); ///< Writes CPU-visible FX SRAM.
    void (*set_irq)(void* context, bool asserted);           ///< Callback used to assert or release the GSU IRQ line.
};

class SuperFx {
   public:
    // Core control

    /// Initializes the SuperFX core with a chip configuration and platform backend.
    void init(const FxConfig& config, const FxBackend& backend);
    /// Returns the emulated GSU state and pipelines to their power-on state.
    void reset();

    // Execution

    /// Advances the core using the timing mode selected by the active chip configuration.
    void run(uint32_t snes_master_clock, uint32_t unlimited_budget);
    /// Advances GSU1/GSU2 against the supplied free-running SNES master-clock count.
    void run_accurate(uint32_t snes_master_clock);
    /// Executes up to the requested number of instructions without wall-clock pacing.
    void run_unlimited(uint32_t instruction_budget);
    /// Executes the currently prefetched opcode and advances R15 when the instruction did not change it.
    void execute();

    // SNES CPU register interface

    /// Reads the SNES-visible GSU register/cache interface.
    uint8_t cpu_read(uint16_t addr);
    /// Writes the SNES-visible GSU register/cache interface and applies start/ownership side effects.
    void cpu_write(uint16_t addr, uint8_t value);

    // SNES CPU ROM / RAM interface

    /// Performs a SNES-side ROM read while honoring original GSU ownership rules.
    uint8_t cpu_rom_read(uint32_t addr);
    /// Performs a SNES-side RAM read while honoring original GSU ownership rules.
    uint8_t cpu_ram_read(uint32_t addr);

    /// Performs a SNES-side RAM write when the CPU currently owns RAM access.
    void cpu_ram_write(uint32_t addr, uint8_t value);

    /// Returns whether the SNES CPU may currently access cartridge ROM.
    bool rom_access_allowed() const;
    /// Returns whether the SNES CPU may currently access GSU RAM.
    bool ram_access_allowed() const;
    /// Returns the open/blocked ROM pattern produced while the GSU owns ROM.
    uint8_t blocked_rom_value(uint32_t addr) const;

    // State access

    /// Returns whether the GSU is currently executing instructions.
    bool running() const { return state_.flags.running; }
    /// Returns a read-only view of the complete emulated GSU state.
    const FxState& state() const { return state_; }
    /// Returns the active chip and timing configuration.
    const FxConfig& config() const { return config_; }

   private:
    // Core state

    FxConfig config_{};                     ///< Active hardware and timing configuration.
    FxBackend backend_{};                   ///< Memory and IRQ interface.
    FxState state_{};                       ///< Complete mutable processor state.

    // Program cache

    uint8_t cache_[512]{};                  ///< 512-byte GSU instruction cache.
    bool cache_valid_[32]{};                ///< Valid flag for each 16-byte cache line.

    // Execution state

    bool r15_changed_ = false;              ///< Prevents the normal PC increment after an explicit R15 write.
    bool wait_for_rom_access_ = false;      ///< Core is stalled until GSU ROM access is granted.
    bool wait_for_ram_access_ = false;      ///< Core is stalled until GSU RAM access is granted.
    bool stopped_ = true;                   ///< Core is currently halted or waiting on an external condition.

    // Accurate timing state

    bool timing_initialized_ = false;       ///< Indicates whether the master-clock timing baseline has been established.
    uint32_t last_master_clock_ = 0;        ///< Previous SNES master-clock sample used to calculate elapsed time.
    uint64_t target_cycles_ = 0;            ///< GSU cycle count the accurate-timing core should run toward.

    // Core execution helpers

    /// Consumes the prefetched opcode and refills the program-read pipeline.
    uint8_t read_opcode();
    /// Consumes one immediate operand byte and advances the program-read pipeline.
    uint8_t read_operand();
    /// Reads the next program byte through the GSU cache or mapped ROM/RAM path.
    uint8_t read_program_byte();
    /// Returns the register selected by the current source prefix state.
    uint16_t read_src() const;

    /// Writes a value through the register selected by the current destination prefix state.
    void write_dst(uint16_t value);
    /// Writes a GSU register while applying the R14 and R15 architectural side effects.
    void write_reg(uint8_t reg, uint16_t value);

    /// Clears ALT/prefix state and restores the default R0 source and destination.
    void reset_prefix();
    /// Recomputes whether instruction execution is stopped by halt or memory ownership.
    void update_running_state();

    // Status register helpers

    /// Packs the low byte of the GSU status register from the internal flag state.
    uint8_t flags_low() const;
    /// Packs the high byte of the GSU status register from the internal flag state.
    uint8_t flags_high() const;

    // Cache

    /// Marks every program-cache line invalid without changing its stored bytes.
    void invalidate_cache();
    /// Fetches one 16-byte program-cache line from the current program bank.
    void fill_cache_line(uint16_t cache_addr);

    // Timing / pipeline

    /// Advances emulated GSU time and retires pending ROM/RAM pipeline operations.
    void step(uint32_t cycles);

    /// Advances time until the current buffered ROM operation can complete.
    void wait_rom_operation();
    /// Advances time until the current delayed RAM write can complete.
    void wait_ram_operation();
    /// Stops execution when the SNES has not granted the GSU ROM ownership.
    void wait_for_rom_access();
    /// Stops execution when the SNES has not granted the GSU RAM ownership.
    void wait_for_ram_access();

    // GSU memory access

    /// Waits for and returns the buffered GSU ROM-read result.
    uint8_t read_rom_buffer();
    /// Reads one byte from the currently selected GSU RAM bank.
    uint8_t read_ram(uint16_t address);

    /// Queues one byte for the delayed GSU RAM-write pipeline.
    void write_ram(uint16_t address, uint8_t value);

    // Opcode decoder

    /// Decodes one opcode byte and dispatches it to the matching GSU instruction handler.
    void execute_opcode(uint8_t opcode);
    /// Handles an opcode that has no implemented GSU operation.
    void unimplemented(uint8_t opcode);

    // Opcodes $00-$3F

    /// Implements STOP, including FX3 completion behavior and the normal GSU IRQ side effect.
    void op_stop();
    /// Implements NOP and clears any pending instruction prefix state.
    void op_nop();
    /// Implements CACHE by aligning R15 to a cache-line base and invalidating on base changes.
    void op_cache();
    /// Implements the logical-right-shift instruction and updates result flags.
    void op_lsr();
    /// Implements rotate-left-through-carry and updates result flags.
    void op_rol();
    /// Applies the signed branch operand when the decoded branch condition is true.
    void op_branch(bool condition);
    /// Implements TO and its WITH-prefixed MOVE form.
    void op_to(uint8_t reg);
    /// Selects the source/destination register used by the following prefixed instruction.
    void op_with(uint8_t reg);
    /// Implements STW/STB using the selected RAM address register.
    void op_store(uint8_t reg);
    /// Implements LOOP using R12 as the counter and R13 as the target.
    void op_loop();
    /// Sets the ALT1 prefix for the next instruction.
    void op_alt1();
    /// Sets the ALT2 prefix for the next instruction.
    void op_alt2();
    /// Sets both ALT prefixes for the next instruction.
    void op_alt3();

    // Opcodes $40-$9F

    /// Implements LDW/LDB from the selected RAM address register.
    void op_load(uint8_t reg);
    /// Dispatches opcode $4C to PLOT or RPIX according to the current ALT state.
    void op_plot_rpix();
    /// Implements SWAP by exchanging the source value bytes.
    void op_swap();
    /// Implements COLOR or CMODE depending on the active ALT prefix.
    void op_color_cmode();
    /// Implements bitwise NOT and updates sign/zero flags.
    void op_not();
    /// Implements ADD/ADC and immediate ADD forms.
    void op_add(uint8_t reg);
    /// Implements SUB/SBC/CMP and immediate subtraction forms.
    void op_sub_compare(uint8_t reg);
    /// Implements normal MERGE or dispatches the FX3 command interface.
    void op_merge();
    /// Implements AND/BIC using a register or immediate nibble operand.
    void op_and_bic(uint8_t reg);
    /// Implements signed/unsigned 8-bit multiplication forms.
    void op_mult(uint8_t reg);
    /// Implements SBK by storing the source word at the current RAM address.
    void op_sbk();
    /// Implements LINK by writing the return address into R11.
    void op_link(uint8_t value);
    /// Implements sign extension of the low source byte.
    void op_sex();
    /// Implements arithmetic shift right and updates carry/sign/zero.
    void op_asr();
    /// Implements rotate right through carry and updates result flags.
    void op_ror();
    /// Implements JMP/LJMP using the selected register target.
    void op_jmp(uint8_t reg);
    /// Implements LOB by isolating the low source byte.
    void op_lob();
    /// Implements FMULT/LMULT with the appropriate result placement and flags.
    void op_fmult_lmult();

    // Opcodes $A0-$FF

    /// Implements the $A0-$AF immediate-byte and short memory-transfer family.
    void op_ibt_sms_lms(uint8_t reg);
    /// Implements FROM and its WITH-prefixed MOVES form.
    void op_from(uint8_t reg);
    /// Implements HIB by returning the high source byte in the low result byte.
    void op_hib();
    /// Implements OR/XOR using register or immediate operands.
    void op_or_xor(uint8_t reg);
    /// Implements register increment without disturbing unrelated status flags.
    void op_inc(uint8_t reg);
    /// Implements GETC/RAMB/ROMB according to the current ALT state.
    void op_getc_ramb_romb();
    /// Implements register decrement without disturbing unrelated status flags.
    void op_dec(uint8_t reg);
    /// Implements GETB/GETBH/GETBL/GETBS from the buffered ROM byte.
    void op_getb();
    /// Implements the $F0-$FF immediate-word and long memory-transfer family.
    void op_iwt_lm_sm(uint8_t reg);

    // Graphics

    /// Applies COLOR register nibble rules to a newly fetched pixel value.
    uint8_t get_color(uint8_t value);
    /// Calculates the GSU tile index for the active screen-height/object mode.
    uint16_t get_tile_index(uint8_t x, uint8_t y);
    /// Calculates the byte address of a GSU tile scanline in planar RAM.
    uint32_t get_tile_address(uint8_t x, uint8_t y);

    /// Reads one GSU pixel from the planar screen buffer.
    uint8_t read_pixel(uint8_t x, uint8_t y);
    /// Draws one pixel through the GSU plot cache (FX3 mode 3 extends the same path to 8bpp).
    void draw_pixel(uint8_t x, uint8_t y);

    /// Returns whether the active color is transparent for the current bit depth.
    bool is_transparent_pixel() const;
    /// Moves the primary plot cache into the secondary writeback slot.
    void flush_primary_cache(uint8_t x, uint8_t y);
    /// Converts cached chunky pixels into GSU bitplanes and writes them to RAM.
    void write_pixel_cache(FxPixelCache& cache);

    // FX3 extensions

    /// Dispatches the FX3 MERGE command number stored in R0.
    void process_fx3_command();
    /// Fills a range of FX3 tile columns with the hardware-compatible clear pattern.
    void fx3_clear(uint8_t first_block, uint8_t last_block);
};
