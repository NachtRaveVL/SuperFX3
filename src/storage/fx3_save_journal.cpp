/*
 * NR-RetroWorks SuperFX3 Firmware
 * Copyright (C) 2026 NR-RetroWorks
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3 or later.
 */

#include "fx3_save_journal.h"

#include <cstring>

namespace fx3_save {
namespace {

constexpr uint32_t MAGIC = 0x33584653u;       // "SFX3" in little-endian storage.
constexpr uint32_t FORMAT_VERSION = 1u;
constexpr uint32_t COMMITTED = 0x54494D43u;   // "CMIT" in little-endian storage.

constexpr uint32_t MAGIC_OFFSET = 0;
constexpr uint32_t VERSION_OFFSET = 4;
constexpr uint32_t HEADER_SIZE_OFFSET = 8;
constexpr uint32_t SEQUENCE_OFFSET = 12;
constexpr uint32_t PAYLOAD_SIZE_OFFSET = 16;
constexpr uint32_t CRC_OFFSET = 20;
constexpr uint32_t COMMIT_OFFSET = 24;

uint32_t read_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

void write_u32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
}

bool range_valid(const QspiFlash& flash, uint32_t offset, uint32_t size) {
    return flash.bytes && offset <= flash.size && size <= flash.size - offset;
}

uint32_t slot_offset(uint32_t slot) {
    return fx3_qspi::SAVE_OFFSET + slot * SLOT_SIZE;
}

bool erased(const uint8_t* data, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        if (data[i] != 0xFF)
            return false;
    }
    return true;
}

bool newer(uint32_t lhs, uint32_t rhs) {
    return static_cast<int32_t>(lhs - rhs) > 0;
}

bool valid_record(const QspiFlash& flash, uint32_t slot, Record& record) {
    const uint32_t offset = slot_offset(slot);
    if (!range_valid(flash, offset, HEADER_SIZE + PAYLOAD_SIZE))
        return false;

    const uint8_t* header = flash.bytes + offset;
    const uint32_t magic = read_u32(header + MAGIC_OFFSET);
    if (magic != MAGIC ||
        read_u32(header + VERSION_OFFSET) != FORMAT_VERSION ||
        read_u32(header + HEADER_SIZE_OFFSET) != HEADER_SIZE ||
        read_u32(header + PAYLOAD_SIZE_OFFSET) != PAYLOAD_SIZE ||
        read_u32(header + COMMIT_OFFSET) != COMMITTED) {
        return false;
    }

    const uint8_t* payload = header + HEADER_SIZE;
    if (crc32(payload, PAYLOAD_SIZE) != read_u32(header + CRC_OFFSET))
        return false;

    record = Record{true, read_u32(header + SEQUENCE_OFFSET), slot};
    return true;
}

bool slot_erased(const QspiFlash& flash, uint32_t slot) {
    const uint32_t offset = slot_offset(slot);
    return range_valid(flash, offset, SLOT_SIZE) && erased(flash.bytes + offset, SLOT_SIZE);
}

class BusyIrqGuard {
public:
    explicit BusyIrqGuard(const QspiFlash& flash) : flash_(flash) {
        flash_.set_busy_irq(flash_.context, true);
    }

    ~BusyIrqGuard() {
        flash_.set_busy_irq(flash_.context, false);
    }

    BusyIrqGuard(const BusyIrqGuard&) = delete;
    BusyIrqGuard& operator=(const BusyIrqGuard&) = delete;

private:
    const QspiFlash& flash_;
};

} // namespace

uint32_t crc32(const uint8_t* data, size_t size) {
    if (!data && size)
        return 0;

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

Record scan(const QspiFlash& flash) {
    Record newest{false, 0, 0};

    for (uint32_t slot = 0; slot < SLOT_COUNT; ++slot) {
        Record candidate{};
        if (!valid_record(flash, slot, candidate))
            continue;
        if (!newest.valid || newer(candidate.sequence, newest.sequence))
            newest = candidate;
    }

    return newest;
}

bool restore(const QspiFlash& flash, uint8_t* destination, size_t destination_size,
             Record* restored_record) {
    if (!destination || destination_size != PAYLOAD_SIZE)
        return false;

    const Record record = scan(flash);
    if (!record.valid)
        return false;

    const uint32_t payload_offset = slot_offset(record.slot) + HEADER_SIZE;
    std::memcpy(destination, flash.bytes + payload_offset, PAYLOAD_SIZE);
    if (restored_record)
        *restored_record = record;
    return true;
}

bool append(const QspiFlash& flash, const uint8_t* snapshot, size_t snapshot_size,
            Record* written_record) {
    if (!snapshot || snapshot_size != PAYLOAD_SIZE || !flash.erase || !flash.program ||
        !flash.set_busy_irq ||
        !range_valid(flash, fx3_qspi::SAVE_OFFSET, fx3_qspi::SAVE_SIZE)) {
        return false;
    }

    const Record previous = scan(flash);
    const uint32_t first_slot = previous.valid ? (previous.slot + 1u) % SLOT_COUNT : 0u;
    uint32_t target_slot = SLOT_COUNT;

    for (uint32_t step = 0; step < SLOT_COUNT; ++step) {
        const uint32_t slot = (first_slot + step) % SLOT_COUNT;
        if (slot_erased(flash, slot)) {
            target_slot = slot;
            break;
        }
    }

    // From the first possible erase/program operation through the commit page,
    // keep /O_IRQ asserted. RAII guarantees release on every failure return.
    BusyIrqGuard busy_irq(flash);

    if (target_slot == SLOT_COUNT) {
        // Reclaim only the next slot, never the newest committed snapshot.
        // SLOT_SIZE is sector-aligned, so an interrupted erase cannot damage it.
        target_slot = first_slot;
        if (!flash.erase(flash.context, slot_offset(target_slot), SLOT_SIZE))
            return false;
    }

    const uint32_t sequence = previous.valid ? previous.sequence + 1u : 0u;
    const uint32_t offset = slot_offset(target_slot);

    // Payload first, header last. A reset or power loss before the final page
    // leaves no committed header, so the partially written slot is skipped.
    if (!flash.program(flash.context, offset + HEADER_SIZE, snapshot, PAYLOAD_SIZE))
        return false;

    uint8_t header[HEADER_SIZE];
    std::memset(header, 0xFF, sizeof(header));
    write_u32(header + MAGIC_OFFSET, MAGIC);
    write_u32(header + VERSION_OFFSET, FORMAT_VERSION);
    write_u32(header + HEADER_SIZE_OFFSET, HEADER_SIZE);
    write_u32(header + SEQUENCE_OFFSET, sequence);
    write_u32(header + PAYLOAD_SIZE_OFFSET, PAYLOAD_SIZE);
    write_u32(header + CRC_OFFSET, crc32(snapshot, snapshot_size));
    write_u32(header + COMMIT_OFFSET, COMMITTED);

    if (!flash.program(flash.context, offset, header, sizeof(header)))
        return false;

    Record verified{};
    if (!valid_record(flash, target_slot, verified) || verified.sequence != sequence)
        return false;

    if (written_record)
        *written_record = Record{true, sequence, target_slot};
    return true;
}

} // namespace fx3_save
