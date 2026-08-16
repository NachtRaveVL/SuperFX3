/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include <atomic>

#include "snes_pio.h"
// Generated from snes_bus.pio by pico_generate_pio_header(); do not hand-maintain.
#include "snes_bus.pio.h"
#include "fx_sync.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "pico.h"
#include "pico/sync.h"
#include "pico/stdlib.h"

#ifndef SNES_FX3
#error "This firmware requires PICO_BOARD=snes_fx3"
#endif

static constexpr uint32_t READ_RESPONSE_DRIVE = 1u;
static constexpr uint32_t READ_RESPONSE_PINDIRS = 0xFFu << 9;

static_assert(NUM_BANK0_GPIOS >= 48, "SuperFX3 requires the 48-GPIO RP2350B package.");
static_assert(NUM_PIOS >= 3, "SuperFX3 requires all three RP2350 PIO blocks.");
static_assert(PICO_PIO_USE_GPIO_BASE == 1, "SuperFX3 requires RP2350B PIO GPIO-base support.");

static std::atomic<bool> g_reset_pending {false};

static SuperFx* g_fx = nullptr;

static uint g_select_sm = 0;
static uint g_write_addr_sm = 0;
static uint g_write_sm = 0;
static uint g_reset_sm = 0;
static uint g_read_sm = 0;

static uint g_select_offset = 0;
static uint g_write_addr_offset = 0;
static uint g_write_offset = 0;
static uint g_reset_offset = 0;
static uint g_read_offset = 0;

static pio_sm_config g_write_addr_config {};
static pio_sm_config g_write_config {};
static pio_sm_config g_read_config {};

static uint g_write_addr_dma = 0;
static dma_channel_config g_write_addr_dma_config {};

static std::atomic<bool> g_pio_started {false};
static std::atomic<bool> g_pio_paused {false};
static std::atomic<bool> g_rom_blocked_requested {false};
static std::atomic<uint32_t> g_rom_ownership_generation {0};
static std::atomic<uint32_t> g_rom_ownership_applied_generation {0};

static critical_section_t g_read_x_gate;
static bool g_rom_blocked_pio = false;

// Address and decode helpers

// Reconstructs A0-A23 from the RP2350B low/high GPIO address groups.
static inline uint32_t snes_read_address(uint64_t gpio) {
    const uint32_t addr_lo = static_cast<uint32_t>(gpio & SNES_ADDR_LO_MASK);
    const uint32_t addr_hi = static_cast<uint32_t>((gpio & SNES_ADDR_HI_MASK) >> SNES_ADDR_HI_BASE);

    return addr_lo | (addr_hi << SNES_ADDR_LO_COUNT);
}

// Returns whether a bank participates in the normal GSU CPU-visible mapping.
static inline bool snes_is_gsu_bank(uint8_t bank) {
    return bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
}

// Returns whether an address is inside the active GSU/FX3 register window.
static inline bool snes_is_gsu_register(const SuperFx& fx, uint32_t address) {
    const uint8_t bank = static_cast<uint8_t>(address >> 16);

    if (!snes_is_gsu_bank(bank)) return false;

    const uint16_t addr = static_cast<uint16_t>(address);

    if (fx.config().chip == FxChip::FX3) {
        // FX3 mirrors the $7000-$72FF block every $400 through $7FFF.
        // The $x300-$x3FF quarter is open bus and must not be driven.
        return addr >= 0x7000 && addr <= 0x7FFF && (addr & 0x0300) != 0x0300;
    }

    return addr >= 0x3000 && addr <= 0x3FFF;
}

// Maps a SNES address to the linear shared-RAM offset used by the core.
static inline bool snes_gsu_ram_offset(const SuperFx& fx, uint32_t address, uint32_t& offset) {
    const uint8_t bank = static_cast<uint8_t>(address >> 16);
    const uint16_t addr = static_cast<uint16_t>(address);

    if (bank == 0x70 || bank == 0x71) {
        offset = (static_cast<uint32_t>(bank - 0x70) << 16) | addr;
        return true;
    }

    if (fx.config().chip == FxChip::FX3)
        return false;

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

// Packs a PIO read response with data and the drive/release direction masks.
static inline uint32_t snes_read_response(uint8_t data) {
    return READ_RESPONSE_DRIVE | (static_cast<uint32_t>(data) << 1) | READ_RESPONSE_PINDIRS;
}

// PIO interrupt handlers

// Answers PIO read requests that cannot be handled by direct ROM pass-through.
static void __not_in_flash_func(snes_read_irq_handler)() {
    if (!pio_interrupt_get(pio2, 0))
        return;

    pio_interrupt_clear(pio2, 0);

    uint32_t response = 0;

    if (g_fx) {
        // FIXME: Latch the SNES address at /RD, or prove the live sample has enough hold time.
        // This handler reads A0-A23 after PIO and Cortex IRQ latency, so correctness currently depends
        // on the SNES keeping the address stable long enough. Measure address hold and /RD-to-data-valid
        // timing before relying on this path.
        const uint64_t gpio = gpio_get_all64();
        const uint32_t address = snes_read_address(gpio);
        const uint16_t addr = static_cast<uint16_t>(address);
        uint32_t ram_offset = 0;

        if (snes_is_gsu_register(*g_fx, address))
            response = snes_read_response(fx_sync_cpu_read(addr));
        else if (snes_gsu_ram_offset(*g_fx, address, ram_offset))
            response = snes_read_response(fx_sync_cpu_ram_read(ram_offset));
        else if (g_fx->config().chip != FxChip::FX3 && !gpio_get(SNES_ROMSEL_N_PIN)) {
            // For GSU1/2, reaching the CPU handler for an ordinary /ROMSEL read
            // means the read SM already classified this transaction as blocked.
            // Do not re-check newer ownership and change the result mid-cycle.
            response = snes_read_response(fx_sync_blocked_rom_value(address));
        }
        
        // NOTE: FX3 $72-$7D /ROMSEL cycles intentionally fall through with response=0.
        // FX3.PDF maps ROM only through $6F and SRAM only at $70-$71, so these
        // cartridge-space reads must not expose the parallel 65816 ROM. Exact
        // open-bus value is system-side behavior; response=0 means do not drive.
    }

    pio_sm_put(pio2, g_read_sm, response);
}

// Consumes captured SNES writes and latches asynchronous reset requests.
static void __not_in_flash_func(snes_control_irq_handler)() {
    if (pio_interrupt_get(pio1, 2)) {
        pio_interrupt_clear(pio1, 2);
        // FIXME: Guarantee that reset is applied before any post-reset write reaches the FX core.
        // PIO latches /RESET immediately, but snes_bus_service() applies it later. Prove the queue
        // and service ordering cannot allow a write after /RESET deassertion to overtake the reset.
        g_reset_pending.store(true, std::memory_order_release);
    }

    if (!pio_interrupt_get(pio1, 1)) return;

    const uint32_t captured = pio_sm_get(pio1, g_write_sm);

    if (g_fx && gpio_get(SNES_RESET_N_PIN)) {
        const uint8_t bank = static_cast<uint8_t>(captured >> SNES_CAPTURE_ADDR_HI_SHIFT);
        const uint8_t data = static_cast<uint8_t>(captured >> SNES_CAPTURE_DATA_SHIFT);
        const uint16_t addr = static_cast<uint16_t>(captured >> SNES_CAPTURE_ADDR_LO_SHIFT);
        const uint32_t address = (static_cast<uint32_t>(bank) << SNES_ADDR_LO_COUNT) | addr;

        uint32_t ram_offset = 0;
        if (snes_is_gsu_register(*g_fx, address)) {
            // FIXME: Bound or eliminate command-queue stalls in the PIO write IRQ.
            // A full ring blocks the current SNES write until core 1 drains it. Measure worst-case
            // back-to-back write service, and remove the remaining circular-wait path for GSU1/2.
            while (!fx_sync_cpu_write(addr, data)) tight_loop_contents();
        } else if (snes_gsu_ram_offset(*g_fx, address, ram_offset)) {
            fx_sync_cpu_ram_write(ram_offset, data);
        }
    }

    pio_interrupt_clear(pio1, 1);
}

// PIO setup and ownership helpers

// Claims one unused state machine and panics if none is available.
static uint snes_claim_sm(PIO pio) {
    const int sm = pio_claim_unused_sm(pio, true);
    return static_cast<uint>(sm);
}

// Loads one generated PIO program into instruction RAM.
static uint snes_add_program(PIO pio, const pio_program_t* program) {
    const int offset = pio_add_program(pio, program);
    if (offset < 0) panic("Unable to load PIO program");
    return static_cast<uint>(offset);
}

// Initializes a PIO state machine and treats an incompatible GPIO window as fatal.
static void snes_init_sm(PIO pio, uint sm, uint offset, const pio_sm_config* config) {
    if (pio_sm_init(pio, sm, offset, config) < 0)
        panic("Unable to initialize PIO state machine");
}

// Starts the endless DMA link from PIO0 address captures to the PIO1 write FIFO.
static void snes_start_write_addr_dma() {
    dma_channel_configure(
        g_write_addr_dma,
        &g_write_addr_dma_config,
        &pio1->txf[g_write_sm],
        &pio0->rxf[g_write_addr_sm],
        dma_encode_endless_transfer_count(),
        true
    );
}

// Returns the GPIO mask for DATA_DIR, /BUS_OE, and /ROM_OE.
static uint64_t snes_control_mask() {
    return SNES_BUS_CTRL_MASK;
}

// Returns the safe PIO control-pin values used while listening to the SNES.
static uint64_t snes_idle_control_values() {
    return (static_cast<uint64_t>(SNES_DATA_DIR_IN) << SNES_DATA_DIR_PIN) |
           (static_cast<uint64_t>(SNES_BUS_ENABLE) << SNES_BUS_OE_N_PIN) |
           (static_cast<uint64_t>(SNES_ROM_DISABLE) << SNES_ROM_OE_N_PIN);
}

// Returns the safe control-pin values used while PIO is disconnected from the bus.
static uint64_t snes_isolated_control_values() {
    return (static_cast<uint64_t>(SNES_DATA_DIR_IN) << SNES_DATA_DIR_PIN) |
           (static_cast<uint64_t>(SNES_BUS_DISABLE) << SNES_BUS_OE_N_PIN) |
           (static_cast<uint64_t>(SNES_ROM_DISABLE) << SNES_ROM_OE_N_PIN);
}

// Updates the read-state-machine scratch value used for ROM ownership/decode.
// The caller must ensure the read SM is stopped or otherwise idle before changing X.
static void snes_set_read_mode_x(bool blocked) {
    if (g_fx->config().chip == FxChip::FX3) {
        // FX3 read PIO compares X against A20-A23 to recognize the low $7x
        // group before enabling the parallel ROM. The CPU handler then maps
        // $70/$71 as SRAM and leaves the rest of that low group undriven.
        pio_sm_exec(
            pio2, g_read_sm,
            pio_encode_set(pio_x, 7)
        );
        g_rom_blocked_pio = false;
        return;
    }

    pio_sm_exec(
        pio2, g_read_sm,
        pio_encode_set(pio_x, blocked ? 1 : 0)
    );

    g_rom_blocked_pio = blocked;
}

// Applies the newest requested GSU ROM ownership value while holding g_read_x_gate.
static void snes_sync_rom_ownership_locked() {
    if (!g_pio_started.load(std::memory_order_acquire) ||
        g_pio_paused.load(std::memory_order_acquire) || !g_fx ||
        g_fx->config().chip == FxChip::FX3) {
        return;
    }

    const uint32_t generation =
        g_rom_ownership_generation.load(std::memory_order_acquire);
    if (generation == g_rom_ownership_applied_generation.load(std::memory_order_relaxed))
        return;

    // Never modify X during a live read. The GSU read program temporarily uses X
    // as an address-decode constant on direct-ROM cycles.
    if (!gpio_get(SNES_RD_N_PIN))
        return;

    const bool blocked = g_rom_blocked_requested.load(std::memory_order_acquire);
    if (blocked == g_rom_blocked_pio) {
        g_rom_ownership_applied_generation.store(generation, std::memory_order_release);
        return;
    }

    // FIXME: Verify that pausing the read SM during an ownership update cannot violate SNES read timing.
    // If /RD falls during the check/disable window we preserve X and resume the same transaction,
    // but the read is still stalled briefly. Measure this case on hardware before calling it safe.
    pio_sm_set_enabled(pio2, g_read_sm, false);

    if (!gpio_get(SNES_RD_N_PIN)) {
        pio_sm_set_enabled(pio2, g_read_sm, true);
        return;
    }

    snes_set_read_mode_x(blocked);
    pio_sm_set_enabled(pio2, g_read_sm, true);
    g_rom_ownership_applied_generation.store(generation, std::memory_order_release);
}

void snes_pio_request_rom_ownership(bool blocked) {
    g_rom_blocked_requested.store(blocked, std::memory_order_relaxed);
    g_rom_ownership_generation.fetch_add(1, std::memory_order_release);

    // Before snes_pio_start() completes there is no initialized gate or live SM;
    // startup will consume the most recently requested value before enabling reads.
    if (!g_pio_started.load(std::memory_order_acquire))
        return;

    snes_pio_sync_rom_ownership();
}

void snes_pio_sync_rom_ownership() {
    if (!g_pio_started.load(std::memory_order_acquire))
        return;

    critical_section_enter_blocking(&g_read_x_gate);
    snes_sync_rom_ownership_locked();
    critical_section_exit(&g_read_x_gate);
}

void snes_pio_pause() {
    critical_section_enter_blocking(&g_read_x_gate);

    // Mark the PIO unavailable before touching the read SM so an ownership
    // notification from core 1 can only record a pending value during pause.
    g_pio_paused.store(true, std::memory_order_release);

    // FIXME: Drain the PIO0 -> DMA -> PIO1 write pipeline before destructive pause in legacy GSU mode.
    // /WR high only proves the external strobe ended; a captured write may still be in the internal
    // pipeline when DMA is aborted and the FIFOs are cleared. FX3 does not use this pause path for ROM.
    pio_sm_set_enabled(pio0, g_write_addr_sm, false);
    pio_sm_set_enabled(pio1, g_write_sm, false);
    pio_sm_set_enabled(pio2, g_read_sm, false);

    dma_channel_abort(g_write_addr_dma);
    pio_sm_clear_fifos(pio0, g_write_addr_sm);
    pio_sm_clear_fifos(pio1, g_write_sm);

    pio_sm_set_pindirs_with_mask64(
        pio2, g_read_sm,
        snes_control_mask(),
        snes_control_mask() | SNES_DATA_MASK
    );

    pio_sm_set_pins_with_mask64(
        pio2, g_read_sm,
        snes_isolated_control_values(),
        snes_control_mask()
    );

    gpio_put(SNES_DATA_DIR_PIN, SNES_DATA_DIR_IN);
    gpio_put(SNES_BUS_OE_N_PIN, SNES_BUS_DISABLE);
    gpio_put(SNES_ROM_OE_N_PIN, SNES_ROM_DISABLE);

    gpio_set_dir(SNES_DATA_DIR_PIN, GPIO_OUT);
    gpio_set_dir(SNES_BUS_OE_N_PIN, GPIO_OUT);
    gpio_set_dir(SNES_ROM_OE_N_PIN, GPIO_OUT);

    gpio_set_function(SNES_DATA_DIR_PIN, GPIO_FUNC_SIO);
    gpio_set_function(SNES_BUS_OE_N_PIN, GPIO_FUNC_SIO);
    gpio_set_function(SNES_ROM_OE_N_PIN, GPIO_FUNC_SIO);

    critical_section_exit(&g_read_x_gate);
}

void snes_pio_resume() {
    critical_section_enter_blocking(&g_read_x_gate);

    pio_interrupt_clear(pio1, 1);
    pio_interrupt_clear(pio2, 0);

    snes_init_sm(pio0, g_write_addr_sm, g_write_addr_offset, &g_write_addr_config);
    snes_init_sm(pio1, g_write_sm, g_write_offset, &g_write_config);
    snes_init_sm(pio2, g_read_sm, g_read_offset, &g_read_config);

    snes_start_write_addr_dma();

    // The read SM is still disabled here, so consume the newest ownership
    // request before reconnecting it to the SNES bus.
    const bool blocked = g_fx->config().chip != FxChip::FX3 &&
                         g_rom_blocked_requested.load(std::memory_order_acquire);
    snes_set_read_mode_x(blocked);
    g_rom_ownership_applied_generation.store(
        g_rom_ownership_generation.load(std::memory_order_acquire),
        std::memory_order_release
    );

    pio_sm_set_pins_with_mask64(
        pio2, g_read_sm,
        snes_isolated_control_values(),
        snes_control_mask()
    );

    pio_sm_set_pindirs_with_mask64(
        pio2, g_read_sm,
        snes_control_mask(),
        snes_control_mask() | SNES_DATA_MASK
    );

    pio_gpio_init(pio2, SNES_DATA_DIR_PIN);
    pio_gpio_init(pio2, SNES_BUS_OE_N_PIN);
    pio_gpio_init(pio2, SNES_ROM_OE_N_PIN);

    pio_sm_set_pins_with_mask64(
        pio2, g_read_sm,
        snes_idle_control_values(),
        snes_control_mask()
    );

    pio_sm_set_enabled(pio0, g_write_addr_sm, true);
    pio_sm_set_enabled(pio1, g_write_sm, true);
    pio_sm_set_enabled(pio2, g_read_sm, true);

    g_pio_paused.store(false, std::memory_order_release);
    critical_section_exit(&g_read_x_gate);
}

// Cartridge bus initialization

void snes_pio_start(SuperFx& fx) {
    g_fx = &fx;

    critical_section_init(&g_read_x_gate);
    g_pio_started.store(false, std::memory_order_relaxed);
    g_pio_paused.store(false, std::memory_order_relaxed);

    // The .pio WAIT GPIO operands use real GPIO numbers. Current Pico SDK relocates
    // them when programs are loaded into a base-16 RP2350B PIO window.
    if (pio_set_gpio_base(pio0, SNES_PIO_ADDR_LO_BASE) < 0 || pio_set_gpio_base(pio1, SNES_PIO_CONTROL_BASE) < 0 || pio_set_gpio_base(pio2, SNES_PIO_CONTROL_BASE) < 0)
        panic("Unable to configure RP2350B PIO GPIO windows");

    g_select_sm = snes_claim_sm(pio0);
    g_write_addr_sm = snes_claim_sm(pio0);
    g_write_sm = snes_claim_sm(pio1);
    g_reset_sm = snes_claim_sm(pio1);
    g_read_sm = snes_claim_sm(pio2);

    pio_sm_config select_config{};

    g_write_addr_offset = snes_add_program(pio0, &snes_write_addr_program);
    g_write_addr_config = snes_write_addr_program_get_default_config(g_write_addr_offset);

    if (fx.config().chip == FxChip::FX3) {
        g_select_offset = snes_add_program(pio0, &snes_select_fx3_program);
        select_config = snes_select_fx3_program_get_default_config(g_select_offset);

        g_write_offset = snes_add_program(pio1, &snes_write_fx3_program);
        g_write_config = snes_write_fx3_program_get_default_config(g_write_offset);

        g_read_offset = snes_add_program(pio2, &snes_read_fx3_program);
        g_read_config = snes_read_fx3_program_get_default_config(g_read_offset);
    } else {
        g_select_offset = snes_add_program(pio0, &snes_select_gsu_program);
        select_config = snes_select_gsu_program_get_default_config(g_select_offset);

        g_write_offset = snes_add_program(pio1, &snes_write_gsu_program);
        g_write_config = snes_write_gsu_program_get_default_config(g_write_offset);

        g_read_offset = snes_add_program(pio2, &snes_read_gsu_program);
        g_read_config = snes_read_gsu_program_get_default_config(g_read_offset);
    }

    g_reset_offset = snes_add_program(pio1, &snes_reset_program);
    pio_sm_config reset_config = snes_reset_program_get_default_config(g_reset_offset);

    sm_config_set_in_pins(&select_config, SNES_A12_PIN);
    sm_config_set_set_pins(&select_config, SNES_SERVICE_SEL_PIN, 1);
    sm_config_set_in_shift(&select_config, false, false, 32);

    sm_config_set_in_pins(&g_write_addr_config, SNES_PIO_ADDR_LO_BASE);
    sm_config_set_in_shift(&g_write_addr_config, false, false, 32);
    sm_config_set_fifo_join(&g_write_addr_config, PIO_FIFO_JOIN_RX);

    sm_config_set_jmp_pin(&g_write_config, SNES_SERVICE_SEL_PIN);
    sm_config_set_in_pins(&g_write_config, SNES_PIO_ADDR_DATA_BASE);
    sm_config_set_in_shift(&g_write_config, false, false, 32);
    sm_config_set_out_shift(&g_write_config, true, false, 32);

    sm_config_set_in_pins(&g_read_config, SNES_ROMSEL_N_PIN);
    sm_config_set_out_pins(&g_read_config, SNES_DATA_BASE, SNES_DATA_COUNT);
    sm_config_set_sideset_pins(&g_read_config, SNES_PIO_SIDESET_BASE);
    sm_config_set_jmp_pin(&g_read_config, SNES_SERVICE_SEL_PIN);
    sm_config_set_in_shift(&g_read_config, false, false, 32);
    sm_config_set_out_shift(&g_read_config, true, false, 32);

    snes_init_sm(pio0, g_select_sm, g_select_offset, &select_config);
    snes_init_sm(pio0, g_write_addr_sm, g_write_addr_offset, &g_write_addr_config);
    snes_init_sm(pio1, g_write_sm, g_write_offset, &g_write_config);
    snes_init_sm(pio1, g_reset_sm, g_reset_offset, &reset_config);
    snes_init_sm(pio2, g_read_sm, g_read_offset, &g_read_config);

    pio_sm_set_pins_with_mask64(pio0, g_select_sm, 0, SNES_SERVICE_SEL_MASK);
    pio_sm_set_pindirs_with_mask64(pio0, g_select_sm, SNES_SERVICE_SEL_MASK, SNES_SERVICE_SEL_MASK);
    pio_gpio_init(pio0, SNES_SERVICE_SEL_PIN);

    const int claimed_write_addr_dma = dma_claim_unused_channel(true);
    if (claimed_write_addr_dma < 0)
        panic("Unable to claim write-address DMA channel");

    g_write_addr_dma = static_cast<uint>(claimed_write_addr_dma);
    g_write_addr_dma_config = dma_channel_get_default_config(g_write_addr_dma);
    channel_config_set_transfer_data_size(&g_write_addr_dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&g_write_addr_dma_config, false);
    channel_config_set_write_increment(&g_write_addr_dma_config, false);
    channel_config_set_high_priority(&g_write_addr_dma_config, true);
    channel_config_set_dreq(
        &g_write_addr_dma_config,
        pio_get_dreq(pio0, g_write_addr_sm, false)
    );
    snes_start_write_addr_dma();

    pio_sm_set_pins_with_mask64(pio2, g_read_sm, snes_idle_control_values(), snes_control_mask());
    pio_sm_set_pindirs_with_mask64(pio2, g_read_sm, snes_control_mask(), snes_control_mask() | SNES_DATA_MASK);

    for (uint8_t pin = SNES_BUS_CTRL_BASE; pin < SNES_BUS_CTRL_BASE + SNES_BUS_CTRL_COUNT; pin++)
        pio_gpio_init(pio2, pin);
    for (uint8_t pin = SNES_DATA_BASE; pin < SNES_DATA_BASE + SNES_DATA_COUNT; pin++)
        pio_gpio_init(pio2, pin);

    pio_set_input_sync_bypass_with_mask64(pio1, SNES_SERVICE_SEL_MASK, SNES_SERVICE_SEL_MASK);
    pio_set_input_sync_bypass_with_mask64(pio2, SNES_SERVICE_SEL_MASK, SNES_SERVICE_SEL_MASK);

    const bool blocked = fx.config().chip != FxChip::FX3 && !fx_sync_rom_access_allowed();
    g_rom_blocked_requested.store(blocked, std::memory_order_release);
    snes_set_read_mode_x(blocked);
    g_rom_ownership_applied_generation.store(
        g_rom_ownership_generation.load(std::memory_order_acquire),
        std::memory_order_release
    );

    pio_interrupt_clear(pio1, 1);
    pio_interrupt_clear(pio1, 2);
    pio_interrupt_clear(pio2, 0);
    pio_set_irq0_source_enabled(pio1, pis_interrupt1, true);
    pio_set_irq0_source_enabled(pio1, pis_interrupt2, true);
    pio_set_irq0_source_enabled(pio2, pis_interrupt0, true);

    const uint pio1_irq = pio_get_irq_num(pio1, 0);
    const uint pio2_irq = pio_get_irq_num(pio2, 0);

    irq_set_exclusive_handler(pio1_irq, snes_control_irq_handler);
    irq_set_exclusive_handler(pio2_irq, snes_read_irq_handler);
    irq_set_enabled(pio1_irq, true);
    irq_set_enabled(pio2_irq, true);

    // Keep local PIO IRQs out of the final enable sequence. This prevents a captured
    // CPU write from changing ownership after X is initialized but before "started"
    // becomes visible to the ownership-notification path.
    critical_section_enter_blocking(&g_read_x_gate);

    pio_sm_set_enabled(pio0, g_select_sm, true);
    pio_sm_set_enabled(pio0, g_write_addr_sm, true);
    pio_sm_set_enabled(pio1, g_reset_sm, true);
    pio_sm_set_enabled(pio1, g_write_sm, true);
    pio_sm_set_enabled(pio2, g_read_sm, true);

    // Publish "started" only after every state machine is live. Runtime ownership
    // notifications can now safely enter g_read_x_gate and touch the read SM.
    g_pio_started.store(true, std::memory_order_release);

    critical_section_exit(&g_read_x_gate);
}

// Reset latch

bool snes_pio_reset_pending() {
    return g_reset_pending.load(std::memory_order_acquire);
}

void snes_pio_clear_reset() {
    g_reset_pending.store(false, std::memory_order_release);
}
