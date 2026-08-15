/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "../cart/cart_bus.h"
#include "fx_bus.h"

static uint8_t fx_ram_read(void* context, uint32_t address) {
    auto* ctx = static_cast<Rp2350FxBusContext*>(context);
    if (!ctx->ram || address >= ctx->ram_size) return 0xFF;

    return ctx->ram[address];
}

static void fx_ram_write(void* context, uint32_t address, uint8_t value) {
    auto* ctx = static_cast<Rp2350FxBusContext*>(context);
    if (!ctx->ram || address >= ctx->ram_size) return;

    ctx->ram[address] = value;
}

static uint8_t fx_rom_read(void* context, uint32_t address) {
    auto* ctx = static_cast<Rp2350FxBusContext*>(context);
    if (address >= ctx->rom_size) return 0xFF;

    return cart_rom_read(context, address);
}

FxBus fx_bus_create(Rp2350FxBusContext* context) {
    FxBus bus{};

    bus.context = context;
    bus.rom_read = fx_rom_read;
    bus.ram_read = fx_ram_read;
    bus.ram_write = fx_ram_write;
    bus.set_irq = cart_irq_write;

    return bus;
}
