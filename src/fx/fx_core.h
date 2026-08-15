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

// -----------------------------------------------------------------------------====
// External memory / IRQ interface
// -----------------------------------------------------------------------------====

struct Fx3Framebuffer {
    uint32_t chunky_base;
    uint16_t chunky_pitch;
    uint32_t planar_base;
};

struct FxBus {
    void* context;

    uint8_t (*rom_read)(void* context, uint32_t address);
    uint8_t (*ram_read)(void* context, uint32_t address);

    void (*ram_write)(void* context, uint32_t address, uint8_t value);
    void (*set_irq)(void* context, bool asserted);
};

// -----------------------------------------------------------------------------====
// Super FX / GSU core
// -----------------------------------------------------------------------------====
class SuperFx {
   public:
    // ------------------------------------------------------------------------
    // Core control
    // ------------------------------------------------------------------------
    void init(const FxConfig& config, const FxBus& bus);
    void reset();

    // ------------------------------------------------------------------------
    // Execution
    // ------------------------------------------------------------------------
    void run(uint32_t snes_master_clock, uint32_t unlimited_budget);
    void run_accurate(uint32_t snes_master_clock);
    void run_unlimited(uint32_t instruction_budget);
    void execute();

    // ------------------------------------------------------------------------
    // SNES CPU register interface
    // ------------------------------------------------------------------------
    uint8_t cpu_read(uint16_t addr);
    void cpu_write(uint16_t addr, uint8_t value);

    // ------------------------------------------------------------------------
    // SNES CPU ROM / RAM interface
    // ------------------------------------------------------------------------
    uint8_t cpu_rom_read(uint32_t addr);
    uint8_t cpu_ram_read(uint32_t addr);
    void cpu_ram_write(uint32_t addr, uint8_t value);

    bool rom_access_allowed() const;
    bool ram_access_allowed() const;
    uint8_t blocked_rom_value(uint32_t addr) const;

    // ------------------------------------------------------------------------
    // State access
    // ------------------------------------------------------------------------
    bool running() const { return state_.flags.running; }
    const FxState& state() const { return state_; }
    const FxConfig& config() const { return config_; }

   private:
    // -----------------------------------------------------------------------------
    // Core state
    // -----------------------------------------------------------------------------
    FxConfig config_{};
    FxBus bus_{};
    FxState state_{};

    // -----------------------------------------------------------------------------
    // Program cache
    // -----------------------------------------------------------------------------
    uint8_t cache_[512]{};
    bool cache_valid_[32]{};

    // -----------------------------------------------------------------------------
    // Execution state
    // -----------------------------------------------------------------------------
    bool r15_changed_ = false;
    bool wait_for_rom_access_ = false;
    bool wait_for_ram_access_ = false;
    bool stopped_ = true;

    // -----------------------------------------------------------------------------
    // Accurate timing state
    // -----------------------------------------------------------------------------
    bool timing_initialized_ = false;
    uint32_t last_master_clock_ = 0;
    uint64_t target_cycles_ = 0;

    // -----------------------------------------------------------------------------
    // Core execution helpers
    // -----------------------------------------------------------------------------
    uint8_t read_opcode();
    uint8_t read_operand();
    uint8_t read_program_byte();
    uint16_t read_src() const;

    void write_dst(uint16_t value);
    void write_reg(uint8_t reg, uint16_t value);

    void reset_prefix();

    void update_running_state();

    // -----------------------------------------------------------------------------
    // Status register helpers
    // -----------------------------------------------------------------------------
    uint8_t flags_low() const;
    uint8_t flags_high() const;

    // -----------------------------------------------------------------------------
    // Cache
    // -----------------------------------------------------------------------------
    void invalidate_cache();
    void fill_cache_line(uint16_t cache_addr);

    // -----------------------------------------------------------------------------
    // Timing / pipeline
    // -----------------------------------------------------------------------------
    void step(uint32_t cycles);
    void wait_rom_operation();
    void wait_ram_operation();
    void wait_for_rom_access();
    void wait_for_ram_access();

    // -----------------------------------------------------------------------------
    // GSU memory access
    // -----------------------------------------------------------------------------
    uint8_t read_rom_buffer();
    uint8_t read_ram(uint16_t address);
    void write_ram(uint16_t address, uint8_t value);

    // -----------------------------------------------------------------------------
    // Opcode decoder
    // -----------------------------------------------------------------------------
    void execute_opcode(uint8_t opcode);
    void unimplemented(uint8_t opcode);

    // -----------------------------------------------------------------------------
    // $00-$3F
    // -----------------------------------------------------------------------------
    void op_stop();
    void op_nop();
    void op_cache();
    void op_lsr();
    void op_rol();
    void op_branch(bool condition);
    void op_to(uint8_t reg);
    void op_with(uint8_t reg);
    void op_store(uint8_t reg);
    void op_loop();
    void op_alt1();
    void op_alt2();
    void op_alt3();

    // -----------------------------------------------------------------------------
    // $40-$9F
    // -----------------------------------------------------------------------------
    void op_load(uint8_t reg);
    void op_plot_rpix();
    void op_swap();
    void op_color_cmode();
    void op_not();
    void op_add(uint8_t reg);
    void op_sub_compare(uint8_t reg);
    void op_merge();
    void op_and_bic(uint8_t reg);
    void op_mult(uint8_t reg);
    void op_sbk();
    void op_link(uint8_t value);
    void op_sex();
    void op_asr();
    void op_ror();
    void op_jmp(uint8_t reg);
    void op_lob();
    void op_fmult_lmult();

    // -----------------------------------------------------------------------------
    // $A0-$FF
    // -----------------------------------------------------------------------------
    void op_ibt_sms_lms(uint8_t reg);
    void op_from(uint8_t reg);
    void op_hib();
    void op_or_xor(uint8_t reg);
    void op_inc(uint8_t reg);
    void op_getc_ramb_romb();
    void op_dec(uint8_t reg);
    void op_getb();
    void op_iwt_lm_sm(uint8_t reg);

    // -----------------------------------------------------------------------------
    // Graphics
    // -----------------------------------------------------------------------------
    uint8_t get_color(uint8_t value);
    uint16_t get_tile_index(uint8_t x, uint8_t y);
    uint32_t get_tile_address(uint8_t x, uint8_t y);
    uint8_t read_pixel(uint8_t x, uint8_t y);
    void draw_pixel(uint8_t x, uint8_t y);
    bool is_transparent_pixel() const;
    void flush_primary_cache(uint8_t x, uint8_t y);
    void write_pixel_cache(FxPixelCache& cache);

    // -----------------------------------------------------------------------------
    // FX3 extensions
    // -----------------------------------------------------------------------------
    void process_fx3_command();
    void fx3_chunky_to_planar(uint8_t region);
    void fx3_clear(uint8_t first_block, uint8_t last_block);
};
