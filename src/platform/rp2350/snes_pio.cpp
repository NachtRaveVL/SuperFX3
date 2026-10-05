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
#include "snes_bus_layout.h"
#include "audio/fx3_audio_stream.h"
#include "video/fx3_video_stream.h"
#include "fx_sync.h"
#include "qspi_bus.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "pico.h"
#include "pico/sync.h"
#include "pico/stdlib.h"
#if SUPERFX3_AUDIO_SD
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "hardware/structs/m33_eppb.h"
#include "hardware/regs/addressmap.h"
#endif

#ifndef SNES_FX3
#error "This firmware requires PICO_BOARD=snes_fx3"
#endif

static constexpr uint32_t READ_RESPONSE_CONTROL_SHIFT = 9;
static constexpr uint32_t READ_RESPONSE_PINDIRS_SHIFT = 16;

static_assert(NUM_BANK0_GPIOS >= 48, "SuperFX3 requires the 48-GPIO RP2350B package.");
static_assert(NUM_PIOS >= 3, "SuperFX3 requires all three RP2350 PIO blocks.");
static_assert(PICO_PIO_USE_GPIO_BASE == 1, "SuperFX3 requires RP2350B PIO GPIO-base support.");

static std::atomic<bool> g_reset_pending {false};
static std::atomic<bool> g_pio_started {false};
static std::atomic<bool> g_pio_paused {true};
static std::atomic<bool> g_rom_blocked {false};

static SuperFx* g_fx = nullptr;
static SnesRomMap g_rom_map = SnesRomMap::Fx3Physical;
static uint32_t g_ram_size = 128u * 1024u;

void snes_pio_set_rom_map(SnesRomMap map, uint32_t ram_size) {
    g_rom_map = map;
    g_ram_size = ram_size <= 128u * 1024u ? ram_size : 128u * 1024u;
}

static uint g_control_sm = 0;
static uint g_write_address_sm = 0;
static uint g_write_trigger_sm = 0;
static uint g_write_capture_sm = 0;
static uint g_reset_sm = 0;
static uint g_read_sm = 0;

static uint g_control_offset = 0;
static uint g_write_address_offset = 0;
static uint g_write_trigger_offset = 0;
static uint g_write_capture_offset = 0;
static uint g_reset_offset = 0;
static uint g_read_offset = 0;

static pio_sm_config g_control_config {};
static pio_sm_config g_write_address_config {};
static pio_sm_config g_write_trigger_config {};
static pio_sm_config g_write_capture_config {};
static pio_sm_config g_reset_config {};
static pio_sm_config g_read_config {};

static uint g_read_control_dma = 0;
static uint g_write_trigger_dma = 0;
static uint g_write_address_dma = 0;
static dma_channel_config g_read_control_dma_config {};
static dma_channel_config g_write_trigger_dma_config {};
static dma_channel_config g_write_address_dma_config {};

static critical_section_t g_pio_gate;

// Returns whether a bank participates in the normal GSU CPU-visible mapping.
static __force_inline bool snes_is_gsu_bank(uint8_t bank) {
    return bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
}

static __force_inline bool snes_has_fx3_audio_map() {
    return g_rom_map == SnesRomMap::Fx3 || g_rom_map == SnesRomMap::Fx3Physical;
}

// Returns whether an address is inside the active GSU/FX3 register window.
static __force_inline bool snes_is_gsu_register(const SuperFx& fx, uint32_t address) {
    const uint8_t bank = static_cast<uint8_t>(address >> 16);
    if (!snes_is_gsu_bank(bank))
        return false;
    if (fx.config().chip == FxChip::FX3 && g_rom_map == SnesRomMap::ExHiRom &&
        (bank & 0x7Fu) >= 0x20u)
        return false; // HiROM SRAM must not be overlaid by FX3 register mirrors.

    const uint16_t addr = static_cast<uint16_t>(address);
    if (fx.config().chip == FxChip::FX3)
        return addr >= 0x7000 && addr <= 0x7FFF && (addr & 0x0300) != 0x0300;

    return addr >= 0x3000 && addr <= 0x3FFF;
}

// Maps a SNES address to the linear shared-RAM offset used by the core.
static __force_inline bool snes_gsu_ram_offset(const SuperFx& fx, uint32_t address, uint32_t& offset) {
    const uint8_t bank = static_cast<uint8_t>(address >> 16);
    const uint16_t addr = static_cast<uint16_t>(address);

    if (fx.config().chip == FxChip::FX3 && g_rom_map == SnesRomMap::ExLoRom) {
        // Extended LoROM needs the upper halves of $70/$71 for unique ROM.
        if ((bank & 0x7Fu) >= 0x70u && (bank & 0x7Fu) <= 0x7Du && addr < 0x8000u) {
            offset = ((static_cast<uint32_t>(bank & 3u) << 15) | addr);
            if (g_ram_size)
                offset &= g_ram_size - 1u;
            return g_ram_size != 0;
        }
        return false;
    }
    if (fx.config().chip == FxChip::FX3 && g_rom_map == SnesRomMap::ExHiRom) {
        if ((bank & 0x7Fu) >= 0x20u && (bank & 0x7Fu) <= 0x3Fu &&
            addr >= 0x6000u && addr < 0x8000u) {
            offset = (static_cast<uint32_t>(bank & 15u) << 13) | (addr & 0x1FFFu);
            if (g_ram_size)
                offset &= g_ram_size - 1u;
            return g_ram_size != 0;
        }
        return false;
    }

    if (bank == 0x70 || bank == 0x71) {
        offset = (static_cast<uint32_t>(bank - 0x70) << 16) | addr;
        if (g_ram_size)
            offset &= g_ram_size - 1u;
        return g_ram_size != 0;
    }

    if (fx.config().chip == FxChip::FX3)
        return false;

    if ((bank <= 0x3E || (bank >= 0x80 && bank <= 0xBE)) &&
        addr >= 0x6000 && addr <= 0x7FFF) {
        offset = addr - 0x6000;
        if (g_ram_size)
            offset &= g_ram_size - 1u;
        return g_ram_size != 0;
    }

    if (bank == 0xF0 || bank == 0xF1) {
        offset = (static_cast<uint32_t>(bank - 0xF0) << 16) | addr;
        if (g_ram_size)
            offset &= g_ram_size - 1u;
        return g_ram_size != 0;
    }
    return false;
}

static __force_inline uint32_t snes_read_response(uint8_t data) {
    return 1u |
           (static_cast<uint32_t>(snes_pack_data_raw(data)) << 1) |
           (static_cast<uint32_t>(SNES_CONTROL_SERVICE_READ) << READ_RESPONSE_CONTROL_SHIFT) |
           (0xFFu << READ_RESPONSE_PINDIRS_SHIFT);
}

static void __not_in_flash_func(snes_read_irq_handler)() {
    if (!pio_interrupt_get(pio2, 0))
        return;

    pio_interrupt_clear(pio2, 0);
    uint32_t response = 0;

    if (g_fx && !g_pio_paused.load(std::memory_order_acquire)) {
        // The read router asserts the direct-ROM path before this handler for an
        // /I_CART-qualified cycle. Address/data stability still requires target
        // timing validation because the full routed address is sampled here.
        const uint64_t gpio = gpio_get_all64();
        const uint32_t address = snes_address_from_gpio(gpio);
        const uint8_t bank = static_cast<uint8_t>(address >> 16);
        const uint16_t addr = static_cast<uint16_t>(address);
        uint32_t ram_offset = 0;

        if (g_fx->config().chip == FxChip::FX3 && snes_has_fx3_audio_map() &&
            snes_is_gsu_bank(bank) &&
            fx3_audio_mmio_address(address)) {
            response = snes_read_response(fx3_audio_host_read(addr));
        } else if (g_fx->config().chip == FxChip::FX3 && snes_has_fx3_audio_map() &&
                   snes_is_gsu_bank(bank) &&
                   fx3_audio_hdma_address(address)) {
            response = snes_read_response(fx3_audio_hdma_read(addr));
        } else if (g_fx->config().chip == FxChip::FX3 && snes_has_fx3_audio_map() &&
                   snes_is_gsu_bank(bank) && fx3_video_mmio_address(address)) {
            response = snes_read_response(fx3_video_host_read(addr));
        } else if (snes_is_gsu_register(*g_fx, address)) {
            response = snes_read_response(fx_sync_cpu_read(addr));
        } else if (snes_gsu_ram_offset(*g_fx, address, ram_offset)) {
            response = snes_read_response(fx_sync_cpu_ram_read(ram_offset));
        } else if (snes_is_gsu_bank(bank) && g_fx->config().chip == FxChip::FX3 &&
                   (addr & 0xF000u) == 0x7000u) {
            response = snes_read_response(0xFF);
        } else if (g_fx->config().chip != FxChip::FX3 &&
                   g_rom_blocked.load(std::memory_order_acquire) &&
                   !gpio_get(SNES_I_CART_N_PIN)) {
            response = snes_read_response(fx_sync_blocked_rom_value(address));
        }
    }

    pio_sm_put(pio2, g_read_sm, response);
}

static void __not_in_flash_func(snes_control_irq_handler)() {
    if (pio_interrupt_get(pio1, 2)) {
        pio_interrupt_clear(pio1, 2);
        g_reset_pending.store(true, std::memory_order_release);
    }

    if (!pio_interrupt_get(pio1, 1))
        return;

    const uint32_t captured = pio_sm_get(pio1, g_write_capture_sm);

    if (g_fx && !g_pio_paused.load(std::memory_order_acquire) &&
        gpio_get(SNES_I_RESET_N_PIN)) {
        while (!snes_pio_service_reset())
            tight_loop_contents();

        const uint32_t raw_address = captured >> SNES_CAPTURE_ADDR_RAW_SHIFT;
        const uint8_t raw_data = static_cast<uint8_t>(captured >> SNES_CAPTURE_DATA_SHIFT);
        const uint32_t address = snes_unpack_address_raw(raw_address);
        const uint8_t data = snes_unpack_data_raw(raw_data);
        const uint16_t addr = static_cast<uint16_t>(address);
        uint32_t ram_offset = 0;

        const uint8_t bank = static_cast<uint8_t>(address >> 16);
        if (g_fx->config().chip == FxChip::FX3 && snes_has_fx3_audio_map() &&
            snes_is_gsu_bank(bank) &&
            fx3_audio_mmio_address(address)) {
            fx3_audio_host_write(addr, data);
        } else if (g_fx->config().chip == FxChip::FX3 && snes_has_fx3_audio_map() &&
                   snes_is_gsu_bank(bank) && fx3_video_mmio_address(address)) {
            fx3_video_host_write(addr, data);
        } else if (snes_is_gsu_register(*g_fx, address)) {
            while (!fx_sync_cpu_write(addr, data))
                tight_loop_contents();
        } else if (snes_gsu_ram_offset(*g_fx, address, ram_offset)) {
            fx_sync_cpu_ram_write(ram_offset, data);
        }
    }

    pio_interrupt_clear(pio1, 1);
}

#if SUPERFX3_AUDIO_SD
static uint64_t g_sd_irq_mask = 0;
static uint32_t g_sd_systick = 0;
static uint32_t g_sd_exception_pending = 0;

static void __not_in_flash_func(snes_sd_read_irq_handler)() {
    if (!pio_interrupt_get(pio2, 0))
        return;
    pio_interrupt_clear(pio2, 0);
    const uint32_t address = snes_address_from_gpio(gpio_get_all64());
    const uint8_t bank = static_cast<uint8_t>(address >> 16);
    const uint16_t addr = static_cast<uint16_t>(address);
    uint32_t offset = 0;
    uint32_t response = 0;
    if (snes_is_gsu_bank(bank) && fx3_audio_mmio_address(address))
        response = snes_read_response(fx3_audio_host_read(addr));
    else if (snes_is_gsu_bank(bank) && fx3_audio_hdma_address(address))
        response = snes_read_response(fx3_audio_hdma_read(addr));
    else if (snes_is_gsu_bank(bank) && fx3_video_mmio_address(address))
        response = snes_read_response(fx3_video_host_read(addr));
    else if (snes_is_gsu_register(*g_fx, address))
        response = snes_read_response(fx_sync_sd_cpu_read(addr));
    else if (snes_gsu_ram_offset(*g_fx, address, offset))
        response = snes_read_response(fx_sync_sd_ram_read(offset));
    else if (snes_is_gsu_bank(bank) && (addr & 0xF000u) == 0x7000u)
        response = snes_read_response(0xFF);
    pio_sm_put(pio2, g_read_sm, response);
}

static void __not_in_flash_func(snes_sd_control_irq_handler)() {
    if (pio_interrupt_get(pio1, 2)) {
        pio_interrupt_clear(pio1, 2);
        fx_sync_sd_reset();
        fx3_audio_request_reset();
        fx3_video_request_reset();
    }
    if (!pio_interrupt_get(pio1, 1))
        return;
    const uint32_t captured = pio_sm_get(pio1, g_write_capture_sm);
    if (gpio_get(SNES_I_RESET_N_PIN)) {
        const uint32_t address = snes_unpack_address_raw(captured >> SNES_CAPTURE_ADDR_RAW_SHIFT);
        const uint8_t data = snes_unpack_data_raw(static_cast<uint8_t>(captured));
        const uint8_t bank = static_cast<uint8_t>(address >> 16);
        const uint16_t addr = static_cast<uint16_t>(address);
        uint32_t offset = 0;
        if (snes_is_gsu_bank(bank) && fx3_audio_mmio_address(address))
            fx3_audio_host_write(addr, data);
        else if (snes_is_gsu_bank(bank) && fx3_video_mmio_address(address))
            fx3_video_host_write(addr, data);
        else if (snes_is_gsu_register(*g_fx, address)) {
            while (!fx_sync_sd_cpu_write(addr, data))
                __compiler_memory_barrier();
        } else if (snes_gsu_ram_offset(*g_fx, address, offset))
            fx_sync_sd_ram_write(offset, data);
    }
    pio_interrupt_clear(pio1, 1);
}

bool snes_pio_sd_begin() {
    if (!g_pio_started.load(std::memory_order_acquire) ||
        g_pio_paused.load(std::memory_order_acquire) ||
        g_reset_pending.load(std::memory_order_acquire) || !snes_has_fx3_audio_map())
        return false;
#ifndef SUPERFX3_TEST
    if (scb_hw->vtor < SRAM_BASE || scb_hw->vtor > SRAM_END - 512u ||
        m33_eppb_hw->nmi_mask[0] || m33_eppb_hw->nmi_mask[1])
        return false;
#endif
    const uint32_t interrupts = save_and_disable_interrupts();
    if (!fx_sync_sd_begin()) {
        restore_interrupts(interrupts);
        return false;
    }
    g_sd_irq_mask = 0;
    for (uint irq = 0; irq < NUM_IRQS; ++irq) {
        if (irq_is_enabled(irq))
            g_sd_irq_mask |= uint64_t{1} << irq;
        irq_set_enabled(irq, false);
    }
    g_sd_systick = systick_hw->csr;
    systick_hw->csr = 0;
    // Disabling SysTick does not clear an already-pending system exception.
    g_sd_exception_pending = scb_hw->icsr &
        (M33_ICSR_PENDSTSET_BITS | M33_ICSR_PENDSVSET_BITS);
    scb_hw->icsr = M33_ICSR_PENDSTCLR_BITS | M33_ICSR_PENDSVCLR_BITS;
    const uint read_irq = pio_get_irq_num(pio2, 0);
    const uint control_irq = pio_get_irq_num(pio1, 0);
    irq_remove_handler(read_irq, snes_read_irq_handler);
    irq_remove_handler(control_irq, snes_control_irq_handler);
    irq_set_exclusive_handler(read_irq, snes_sd_read_irq_handler);
    irq_set_exclusive_handler(control_irq, snes_sd_control_irq_handler);
    irq_set_enabled(read_irq, true);
    irq_set_enabled(control_irq, true);
    restore_interrupts(interrupts);
    return true;
}

void snes_pio_sd_end() {
    const uint32_t interrupts = save_and_disable_interrupts();
    const uint read_irq = pio_get_irq_num(pio2, 0);
    const uint control_irq = pio_get_irq_num(pio1, 0);
    irq_remove_handler(read_irq, snes_sd_read_irq_handler);
    irq_remove_handler(control_irq, snes_sd_control_irq_handler);
    irq_set_exclusive_handler(read_irq, snes_read_irq_handler);
    irq_set_exclusive_handler(control_irq, snes_control_irq_handler);
    systick_hw->csr = g_sd_systick;
    scb_hw->icsr = g_sd_exception_pending;
    for (uint irq = 0; irq < NUM_IRQS; ++irq)
        irq_set_enabled(irq, (g_sd_irq_mask & (uint64_t{1} << irq)) != 0);
    restore_interrupts(interrupts);
}
#endif

static uint snes_claim_sm(PIO pio) {
    const int sm = pio_claim_unused_sm(pio, true);
    return static_cast<uint>(sm);
}

static uint snes_add_program(PIO pio, const pio_program_t* program) {
    const int offset = pio_add_program(pio, program);
    if (offset < 0)
        panic("Unable to load PIO program");
    return static_cast<uint>(offset);
}

static void snes_init_sm(PIO pio, uint sm, uint offset, const pio_sm_config* config) {
    if (pio_sm_init(pio, sm, offset, config) < 0)
        panic("Unable to initialize PIO state machine");
}

static uint snes_claim_dma() {
    const int channel = dma_claim_unused_channel(true);
    if (channel < 0)
        panic("Unable to claim SNES bus DMA channel");
    return static_cast<uint>(channel);
}

static dma_channel_config snes_dma_config(uint channel, uint dreq) {
    dma_channel_config config = dma_channel_get_default_config(channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, false);
    channel_config_set_high_priority(&config, true);
    channel_config_set_dreq(&config, dreq);
    return config;
}

static void snes_start_dma() {
    dma_channel_configure(
        g_read_control_dma, &g_read_control_dma_config,
        &pio0->txf[g_control_sm], &pio2->rxf[g_read_sm],
        dma_encode_endless_transfer_count(), true
    );
    dma_channel_configure(
        g_write_trigger_dma, &g_write_trigger_dma_config,
        &pio0->txf[g_write_address_sm], &pio1->rxf[g_write_trigger_sm],
        dma_encode_endless_transfer_count(), true
    );
    dma_channel_configure(
        g_write_address_dma, &g_write_address_dma_config,
        &pio1->txf[g_write_capture_sm], &pio0->rxf[g_write_address_sm],
        dma_encode_endless_transfer_count(), true
    );
}

static void snes_abort_dma() {
    dma_channel_abort(g_read_control_dma);
    dma_channel_abort(g_write_trigger_dma);
    dma_channel_abort(g_write_address_dma);
}

static void snes_local_control_sio(uint8_t word) {
    const uint64_t values = static_cast<uint64_t>(word) << SNES_LOCAL_CONTROL_BASE;
    gpio_put_masked64(SNES_LOCAL_CONTROL_MASK, values);
    gpio_set_dir_masked64(SNES_LOCAL_CONTROL_MASK, SNES_LOCAL_CONTROL_MASK);
    for (uint pin = SNES_LOCAL_CONTROL_BASE;
         pin < SNES_LOCAL_CONTROL_BASE + SNES_LOCAL_CONTROL_COUNT; ++pin) {
        gpio_set_function(pin, GPIO_FUNC_SIO);
    }
}

static void snes_disable_transaction_sms(bool keep_reset) {
    pio_sm_set_enabled(pio0, g_control_sm, false);
    pio_sm_set_enabled(pio0, g_write_address_sm, false);
    pio_sm_set_enabled(pio1, g_write_trigger_sm, false);
    pio_sm_set_enabled(pio1, g_write_capture_sm, false);
    pio_sm_set_enabled(pio2, g_read_sm, false);
    if (!keep_reset)
        pio_sm_set_enabled(pio1, g_reset_sm, false);
}

void snes_pio_request_rom_ownership(bool blocked) {
    g_rom_blocked.store(blocked, std::memory_order_release);
}

void snes_pio_sync_rom_ownership() {
    // The routed read path snapshots this state in the CPU service handler.
}

void snes_pio_pause() {
    if (!g_pio_started.load(std::memory_order_acquire))
        return;

    critical_section_enter_blocking(&g_pio_gate);
    g_pio_paused.store(true, std::memory_order_release);
    snes_disable_transaction_sms(true);
    snes_abort_dma();

    pio_sm_clear_fifos(pio0, g_control_sm);
    pio_sm_clear_fifos(pio0, g_write_address_sm);
    pio_sm_clear_fifos(pio1, g_write_trigger_sm);
    pio_sm_clear_fifos(pio1, g_write_capture_sm);
    pio_sm_clear_fifos(pio2, g_read_sm);

    gpio_set_dir_masked64(SNES_DATA_MASK, 0);
    for (uint pin = SNES_DATA_RAW_BASE; pin < SNES_DATA_RAW_BASE + SNES_DATA_RAW_COUNT; ++pin)
        gpio_set_function(pin, GPIO_FUNC_SIO);
    snes_local_control_sio(SNES_CONTROL_BUS_ISOLATED);
    critical_section_exit(&g_pio_gate);
}

void snes_pio_stop() {
    if (!g_pio_started.load(std::memory_order_acquire))
        return;

    snes_pio_pause();
    critical_section_enter_blocking(&g_pio_gate);
    pio_sm_set_enabled(pio1, g_reset_sm, false);
    critical_section_exit(&g_pio_gate);
}

void snes_pio_resume() {
    if (!g_pio_started.load(std::memory_order_acquire))
        return;

    critical_section_enter_blocking(&g_pio_gate);
    if (!g_pio_paused.load(std::memory_order_acquire)) {
        critical_section_exit(&g_pio_gate);
        return;
    }

    pio_interrupt_clear(pio1, 1);
    pio_interrupt_clear(pio1, 2);
    pio_interrupt_clear(pio2, 0);

    snes_init_sm(pio0, g_control_sm, g_control_offset, &g_control_config);
    snes_init_sm(pio0, g_write_address_sm, g_write_address_offset, &g_write_address_config);
    snes_init_sm(pio1, g_write_trigger_sm, g_write_trigger_offset, &g_write_trigger_config);
    snes_init_sm(pio1, g_write_capture_sm, g_write_capture_offset, &g_write_capture_config);
    snes_init_sm(pio1, g_reset_sm, g_reset_offset, &g_reset_config);
    snes_init_sm(pio2, g_read_sm, g_read_offset, &g_read_config);

    snes_start_dma();

    pio_sm_set_pins_with_mask64(
        pio0, g_control_sm,
        static_cast<uint64_t>(SNES_CONTROL_CONSOLE_IDLE) << SNES_LOCAL_CONTROL_BASE,
        SNES_LOCAL_CONTROL_MASK
    );
    pio_sm_set_pindirs_with_mask64(
        pio0, g_control_sm, SNES_LOCAL_CONTROL_MASK, SNES_LOCAL_CONTROL_MASK
    );
    pio_sm_set_pindirs_with_mask64(pio2, g_read_sm, 0, SNES_DATA_MASK);

    for (uint pin = SNES_LOCAL_CONTROL_BASE;
         pin < SNES_LOCAL_CONTROL_BASE + SNES_LOCAL_CONTROL_COUNT; ++pin) {
        pio_gpio_init(pio0, pin);
    }
    for (uint pin = SNES_DATA_RAW_BASE; pin < SNES_DATA_RAW_BASE + SNES_DATA_RAW_COUNT; ++pin)
        pio_gpio_init(pio2, pin);

    // X is the console-listening control word pushed after every /I_RD release.
    pio_sm_exec(pio2, g_read_sm, pio_encode_set(pio_x, SNES_CONTROL_CONSOLE_IDLE));

    pio_sm_set_enabled(pio0, g_control_sm, true);
    pio_sm_set_enabled(pio0, g_write_address_sm, true);
    pio_sm_set_enabled(pio1, g_write_trigger_sm, true);
    pio_sm_set_enabled(pio1, g_write_capture_sm, true);
    pio_sm_set_enabled(pio1, g_reset_sm, true);
    pio_sm_set_enabled(pio2, g_read_sm, true);

    g_pio_paused.store(false, std::memory_order_release);
    critical_section_exit(&g_pio_gate);
}

void snes_pio_start(SuperFx& fx) {
    g_fx = &fx;
    critical_section_init(&g_pio_gate);
    g_pio_started.store(false, std::memory_order_relaxed);
    g_pio_paused.store(true, std::memory_order_relaxed);
    g_reset_pending.store(false, std::memory_order_relaxed);

    if (pio_set_gpio_base(pio0, SNES_PIO_LOWER_BASE) < 0 ||
        pio_set_gpio_base(pio1, SNES_PIO_UPPER_BASE) < 0 ||
        pio_set_gpio_base(pio2, SNES_PIO_UPPER_BASE) < 0) {
        panic("Unable to configure RP2350B PIO GPIO windows");
    }

    g_control_sm = snes_claim_sm(pio0);
    g_write_address_sm = snes_claim_sm(pio0);
    g_write_trigger_sm = snes_claim_sm(pio1);
    g_write_capture_sm = snes_claim_sm(pio1);
    g_reset_sm = snes_claim_sm(pio1);
    g_read_sm = snes_claim_sm(pio2);

    g_control_offset = snes_add_program(pio0, &snes_control_output_program);
    g_write_address_offset = snes_add_program(pio0, &snes_write_address_program);
    g_write_trigger_offset = snes_add_program(pio1, &snes_write_trigger_program);
    g_write_capture_offset = snes_add_program(pio1, &snes_write_capture_program);
    g_reset_offset = snes_add_program(pio1, &snes_reset_program);
    g_read_offset = snes_add_program(pio2, &snes_read_program);

    g_control_config = snes_control_output_program_get_default_config(g_control_offset);
    g_write_address_config = snes_write_address_program_get_default_config(g_write_address_offset);
    g_write_trigger_config = snes_write_trigger_program_get_default_config(g_write_trigger_offset);
    g_write_capture_config = snes_write_capture_program_get_default_config(g_write_capture_offset);
    g_reset_config = snes_reset_program_get_default_config(g_reset_offset);
    g_read_config = snes_read_program_get_default_config(g_read_offset);

    sm_config_set_out_pins(&g_control_config, SNES_LOCAL_CONTROL_BASE, SNES_LOCAL_CONTROL_COUNT);
    sm_config_set_out_shift(&g_control_config, true, false, 32);

    sm_config_set_in_pins(&g_write_address_config, SNES_ADDR_RAW_BASE);
    sm_config_set_in_shift(&g_write_address_config, false, false, 32);

    sm_config_set_in_pins(&g_write_capture_config, SNES_DATA_RAW_BASE);
    sm_config_set_in_shift(&g_write_capture_config, false, false, 32);
    sm_config_set_out_shift(&g_write_capture_config, true, false, 32);

    sm_config_set_jmp_pin(&g_read_config, SNES_I_CART_N_PIN);
    sm_config_set_out_pins(&g_read_config, SNES_DATA_RAW_BASE, SNES_DATA_RAW_COUNT);
    sm_config_set_in_shift(&g_read_config, false, false, 32);
    sm_config_set_out_shift(&g_read_config, true, false, 32);

    snes_init_sm(pio0, g_control_sm, g_control_offset, &g_control_config);
    snes_init_sm(pio0, g_write_address_sm, g_write_address_offset, &g_write_address_config);
    snes_init_sm(pio1, g_write_trigger_sm, g_write_trigger_offset, &g_write_trigger_config);
    snes_init_sm(pio1, g_write_capture_sm, g_write_capture_offset, &g_write_capture_config);
    snes_init_sm(pio1, g_reset_sm, g_reset_offset, &g_reset_config);
    snes_init_sm(pio2, g_read_sm, g_read_offset, &g_read_config);

    g_read_control_dma = snes_claim_dma();
    g_write_trigger_dma = snes_claim_dma();
    g_write_address_dma = snes_claim_dma();
    g_read_control_dma_config = snes_dma_config(
        g_read_control_dma, pio_get_dreq(pio2, g_read_sm, false)
    );
    g_write_trigger_dma_config = snes_dma_config(
        g_write_trigger_dma, pio_get_dreq(pio1, g_write_trigger_sm, false)
    );
    g_write_address_dma_config = snes_dma_config(
        g_write_address_dma, pio_get_dreq(pio0, g_write_address_sm, false)
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

    g_pio_started.store(true, std::memory_order_release);
}

bool snes_pio_reset_pending() {
    return g_reset_pending.load(std::memory_order_acquire);
}

bool __not_in_flash_func(snes_pio_service_reset)() {
    const uint32_t irq_state = save_and_disable_interrupts();
    bool accepted = true;
    if (g_reset_pending.load(std::memory_order_acquire)) {
        accepted = fx_sync_reset();
        if (accepted) {
            fx3_audio_request_reset();
            fx3_video_request_reset();
            g_reset_pending.store(false, std::memory_order_release);
        }
    }
    restore_interrupts(irq_state);
    return accepted;
}
