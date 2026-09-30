/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "parallel_rom_programmer.h"

namespace {
constexpr uint32_t UNLOCK_AAA = 0x000AAAu;
constexpr uint32_t UNLOCK_555 = 0x000555u;

constexpr uint8_t CMD_RESET = 0xF0;
constexpr uint8_t CMD_AUTOSELECT = 0x90;
constexpr uint8_t CMD_PROGRAM = 0xA0;
constexpr uint8_t CMD_ERASE_SETUP = 0x80;
constexpr uint8_t CMD_CHIP_ERASE = 0x10;
constexpr uint8_t CMD_SECTOR_ERASE = 0x30;

constexpr uint8_t DQ7 = 0x80;
constexpr uint8_t DQ5 = 0x20;
}

ParallelRomProgrammer::ParallelRomProgrammer(const ParallelRomBus& bus) : bus_(bus) {}

void ParallelRomProgrammer::command(uint32_t address, uint8_t data) const {
    bus_.write(bus_.context, address, data);
}

void ParallelRomProgrammer::reset() const {
    command(0, CMD_RESET);
}

ParallelRomId ParallelRomProgrammer::read_id() const {
    command(UNLOCK_AAA, 0xAA);
    command(UNLOCK_555, 0x55);
    command(UNLOCK_AAA, CMD_AUTOSELECT);

    // IS29GL128 byte-mode ID locations from the ISSI command table.
    const ParallelRomId id {
        bus_.read(bus_.context, 0x000000u),
        bus_.read(bus_.context, 0x000002u),
        bus_.read(bus_.context, 0x00001Cu),
        bus_.read(bus_.context, 0x00001Eu),
    };
    reset();
    return id;
}

bool ParallelRomProgrammer::is_supported_device() const {
    const ParallelRomId id = read_id();
    return id.manufacturer == 0x9D && id.device_code_1 == 0x7E &&
           id.device_code_2 == 0x21 && id.device_code_3 == 0x01;
}

bool ParallelRomProgrammer::wait_ready(uint32_t address, uint8_t expected,
                                       uint64_t timeout_us) const {
    const uint64_t start = bus_.time_us(bus_.context);
    while (bus_.time_us(bus_.context) - start <= timeout_us) {
        uint8_t status = bus_.read(bus_.context, address);
        if (((status ^ expected) & DQ7) == 0)
            return true;

        if (status & DQ5) {
            // DQ5 can rise at the same instant that the operation completes.
            status = bus_.read(bus_.context, address);
            return ((status ^ expected) & DQ7) == 0;
        }
        if (bus_.service)
            bus_.service(bus_.context);
    }
    return false;
}

bool ParallelRomProgrammer::erase_chip(uint64_t timeout_us) const {
    command(UNLOCK_AAA, 0xAA);
    command(UNLOCK_555, 0x55);
    command(UNLOCK_AAA, CMD_ERASE_SETUP);
    command(UNLOCK_AAA, 0xAA);
    command(UNLOCK_555, 0x55);
    command(UNLOCK_AAA, CMD_CHIP_ERASE);
    const bool ok = wait_ready(0, 0xFF, timeout_us);
    if (!ok)
        reset();
    return ok;
}

bool ParallelRomProgrammer::erase_sector(uint32_t address, uint64_t timeout_us) const {
    address &= ~(SECTOR_SIZE - 1u);
    command(UNLOCK_AAA, 0xAA);
    command(UNLOCK_555, 0x55);
    command(UNLOCK_AAA, CMD_ERASE_SETUP);
    command(UNLOCK_AAA, 0xAA);
    command(UNLOCK_555, 0x55);
    command(address, CMD_SECTOR_ERASE);
    const bool ok = wait_ready(address, 0xFF, timeout_us);
    if (!ok)
        reset();
    return ok;
}

bool ParallelRomProgrammer::program_byte(uint32_t address, uint8_t data,
                                         uint64_t timeout_us) const {
    if (address >= CAPACITY)
        return false;
    if (data == 0xFF)
        return bus_.read(bus_.context, address) == 0xFF;

    command(UNLOCK_AAA, 0xAA);
    command(UNLOCK_555, 0x55);
    command(UNLOCK_AAA, CMD_PROGRAM);
    command(address, data);
    const bool ok = wait_ready(address, data, timeout_us);
    if (!ok)
        reset();
    return ok && bus_.read(bus_.context, address) == data;
}

bool ParallelRomProgrammer::verify(uint32_t address, const uint8_t* data,
                                   size_t size) const {
    if (!data || address > CAPACITY || size > CAPACITY - address)
        return false;
    for (size_t index = 0; index < size; ++index) {
        if (bus_.read(bus_.context, address + static_cast<uint32_t>(index)) != data[index])
            return false;
        if (bus_.service && (index & 0xFFu) == 0)
            bus_.service(bus_.context);
    }
    return true;
}
