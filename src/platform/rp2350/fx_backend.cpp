/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "fx_backend.h"

#include "../../storage/fx3_qspi_layout.h"
#include "snes_bus.h"

#include "hardware/regs/addressmap.h"
#include "pico.h"

#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES must describe the RP2350 QSPI flash size"
#endif

static_assert(PICO_FLASH_SIZE_BYTES == fx3_qspi::FLASH_SIZE,
              "The production SuperFX3 layout requires the 4 MiB W25Q32 QSPI device.");

extern "C" const uint8_t __flash_binary_end;

bool fx3_qspi_rom_init(Rp2350FxBackendContext& context) {
    const uintptr_t rom_start = static_cast<uintptr_t>(XIP_BASE) + fx3_qspi::FX_CODE_OFFSET;
    const uintptr_t save_start = static_cast<uintptr_t>(XIP_BASE) + fx3_qspi::SAVE_OFFSET;
    const uintptr_t firmware_end = reinterpret_cast<uintptr_t>(&__flash_binary_end);

    // Firmware, the save journal, and FX-visible ROM share the primary QSPI device.
    // The external SNES game/program ROM is not mapped into this device.
    // Refuse to boot if the linked firmware has grown into persistent save data.
    if (firmware_end > save_start)
        return false;

    context.rom = reinterpret_cast<const uint8_t*>(rom_start);
    context.rom_size = fx3_qspi::FX_CODE_SIZE;
    return true;
}

// Reads one byte from the linear GSU-visible ROM reserved in primary QSPI flash.
uint8_t __not_in_flash_func(fx3_qspi_rom_read)(void* context, uint32_t offset) {
    auto* ctx = static_cast<Rp2350FxBackendContext*>(context);
    if (!ctx || !ctx->rom || offset >= ctx->rom_size)
        return 0xFF;

    return ctx->rom[offset];
}

// Forwards a linear core ROM offset to the configured FX-ROM backend.
static uint8_t __not_in_flash_func(fx_rom_read)(void* context, uint32_t offset) {
    auto* ctx = static_cast<Rp2350FxBackendContext*>(context);
    if (!ctx || !ctx->rom_read)
        return 0xFF;

    return ctx->rom_read(ctx, offset);
}

// Reads one shared RAM byte through a lock-free atomic load.
static uint8_t __not_in_flash_func(fx_ram_read)(void* context, uint32_t address) {
    auto* ctx = static_cast<Rp2350FxBackendContext*>(context);
    if (!ctx || !ctx->ram || address >= ctx->ram_size)
        return 0xFF;

    return ctx->ram[address].load(std::memory_order_relaxed);
}

// Writes one shared RAM byte through a lock-free atomic store.
static void __not_in_flash_func(fx_ram_write)(void* context, uint32_t address, uint8_t value) {
    auto* ctx = static_cast<Rp2350FxBackendContext*>(context);
    if (!ctx || !ctx->ram || address >= ctx->ram_size)
        return;

    ctx->ram[address].store(value, std::memory_order_relaxed);
}

// Forwards the core IRQ state to the cartridge-edge IRQ driver.
static void __not_in_flash_func(fx_set_irq)(void* context, bool asserted) {
    auto* ctx = static_cast<Rp2350FxBackendContext*>(context);

    if (ctx && ctx->irq_write) ctx->irq_write(ctx, asserted);
}

FxBackend fx_backend_create(Rp2350FxBackendContext* context) {
    FxBackend backend{};

    backend.context = context;
    backend.rom_read = fx_rom_read;
    // cpu_rom_read remains null because SNES CPU ROM reads stay on the parallel-ROM/PIO path.
    backend.ram_read = fx_ram_read;
    backend.ram_write = fx_ram_write;
    backend.set_irq = fx_set_irq;

    return backend;
}

#if SUPERFX3_AUDIO_SD
bool fx_backend_sd_safe(const FxBackend& backend) {
    const uintptr_t context = reinterpret_cast<uintptr_t>(backend.context);
    if (context < SRAM_BASE || context > SRAM_END - sizeof(Rp2350FxBackendContext))
        return false;
    const auto* ctx = static_cast<const Rp2350FxBackendContext*>(backend.context);
    const uintptr_t ram = reinterpret_cast<uintptr_t>(ctx->ram);
    return ram >= SRAM_BASE && ram < SRAM_END && ctx->ram_size <= SRAM_END - ram &&
        backend.ram_read == fx_ram_read && backend.ram_write == fx_ram_write &&
        backend.set_irq == fx_set_irq && ctx->irq_write == snes_irq_write;
}
#endif
