/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "fx_backend.h"

#include "hardware/regs/addressmap.h"
#include "pico.h"

static constexpr uint32_t FX3_QSPI_ROM_SIZE = 3u * 1024u * 1024u; ///< Reserved FX3 ROM image size.

#ifndef PICO_FLASH_SIZE_BYTES
#error "PICO_FLASH_SIZE_BYTES must describe the RP2350 QSPI flash size"
#endif

// Keep the FX ROM at the top of the primary QSPI flash so the firmware can grow
// upward from offset 0 without needing a custom linker section for the ROM image.
// A board build can override this when its flash layout needs a fixed partition.
#ifndef FX3_QSPI_ROM_OFFSET
#define FX3_QSPI_ROM_OFFSET (PICO_FLASH_SIZE_BYTES - FX3_QSPI_ROM_SIZE)
#endif

static_assert(PICO_FLASH_SIZE_BYTES >= 4u * 1024u * 1024u,
              "FX3 requires at least 4 MiB of QSPI flash for firmware plus the 3 MiB ROM.");
static_assert((FX3_QSPI_ROM_OFFSET & 0xFFFu) == 0,
              "The FX3 QSPI ROM partition must start on a 4 KiB flash sector boundary.");
static_assert(FX3_QSPI_ROM_OFFSET + FX3_QSPI_ROM_SIZE <= PICO_FLASH_SIZE_BYTES,
              "The FX3 QSPI ROM partition extends beyond the configured flash size.");

extern "C" const uint8_t __flash_binary_end;

bool fx3_qspi_rom_init(Rp2350FxBackendContext& context) {
    const uintptr_t rom_start = static_cast<uintptr_t>(XIP_BASE) + FX3_QSPI_ROM_OFFSET;
    const uintptr_t firmware_end = reinterpret_cast<uintptr_t>(&__flash_binary_end);

    // Firmware and FX ROM share the primary QSPI device. Refuse to boot if the
    // firmware has grown into the reserved ROM partition.
    if (firmware_end > rom_start)
        return false;

    context.rom = reinterpret_cast<const uint8_t*>(rom_start);
    context.rom_size = FX3_QSPI_ROM_SIZE;
    return true;
}

// Reads one byte from the linear FX3 ROM image reserved in primary QSPI flash.
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
