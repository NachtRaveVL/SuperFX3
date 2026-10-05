/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

struct ParallelRomBus {
    void* context;
    uint8_t (*read)(void* context, uint32_t address);
    void (*write)(void* context, uint32_t address, uint8_t data);
    uint64_t (*time_us)(void* context);
    void (*service)(void* context);
};

struct ParallelRomId {
    uint8_t manufacturer;
    uint8_t device_code_1;
    uint8_t device_code_2;
    uint8_t device_code_3;
};

class ParallelRomProgrammer {
public:
    static constexpr uint32_t MIN_CAPACITY = 1u * 1024u * 1024u;
    static constexpr uint32_t MAX_CAPACITY = 16u * 1024u * 1024u;
    static constexpr uint32_t SECTOR_SIZE = 128u * 1024u;

    explicit ParallelRomProgrammer(const ParallelRomBus& bus);

    void reset() const;
    ParallelRomId read_id() const;
    bool probe();
    bool is_supported_device() { return probe(); }
    uint32_t capacity() const { return capacity_; }
    bool erase_chip(uint64_t timeout_us = 180000000u) const;
    bool erase_sector(uint32_t address, uint64_t timeout_us = 2000000u) const;
    bool program_byte(uint32_t address, uint8_t data,
                      uint64_t timeout_us = 1000u) const;
    bool verify(uint32_t address, const uint8_t* data, size_t size) const;

private:
    bool wait_ready(uint32_t address, uint8_t expected, uint64_t timeout_us) const;
    void command(uint32_t address, uint8_t data) const;

    ParallelRomBus bus_;
    uint32_t capacity_ = 0;
};
