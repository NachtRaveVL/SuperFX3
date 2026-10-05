#include <cstdint>
#include <cstdio>
#include <deque>
#include <utility>
#include <vector>

#include "../storage/parallel_rom_programmer.h"
#include "test_support.h"

struct FakeRom {
    std::vector<std::pair<uint32_t, uint8_t>> writes;
    std::deque<uint8_t> reads;
    uint64_t now = 0;
    unsigned service_count = 0;
};

static uint8_t fake_read(void* context, uint32_t) {
    auto& rom = *static_cast<FakeRom*>(context);
    ++rom.now;
    if (rom.reads.empty())
        return 0xFF;
    const uint8_t value = rom.reads.front();
    rom.reads.pop_front();
    return value;
}

static void fake_write(void* context, uint32_t address, uint8_t data) {
    static_cast<FakeRom*>(context)->writes.emplace_back(address, data);
}

static uint64_t fake_time(void* context) {
    return static_cast<FakeRom*>(context)->now++;
}

static void fake_service(void* context) {
    ++static_cast<FakeRom*>(context)->service_count;
}

static ParallelRomProgrammer programmer(FakeRom& rom) {
    return ParallelRomProgrammer({&rom, fake_read, fake_write, fake_time, fake_service});
}

static void probe(ParallelRomProgrammer& flash, FakeRom& rom, uint8_t size_power) {
    const uint16_t sectors = static_cast<uint16_t>((1u << size_power) /
        ParallelRomProgrammer::SECTOR_SIZE);
    rom.reads = {'Q', 'R', 'Y', 2, 0, size_power, 1,
                 static_cast<uint8_t>(sectors - 1u),
                 static_cast<uint8_t>((sectors - 1u) >> 8), 0, 2};
    test_require(flash.probe(), "valid uniform CFI geometry was rejected");
    test_require(flash.capacity() == (1u << size_power), "CFI capacity was decoded incorrectly");
    rom.writes.clear();
}

static void require_writes(const FakeRom& rom,
                           std::initializer_list<std::pair<uint32_t, uint8_t>> expected,
                           const char* message) {
    test_require(rom.writes == std::vector<std::pair<uint32_t, uint8_t>>(expected), message);
}

static void test_byte_program_sequence() {
    FakeRom rom;
    auto flash = programmer(rom);
    probe(flash, rom, 24);
    rom.reads = {0x05, 0xA5, 0xA5};
    test_require(flash.program_byte(0x123456, 0xA5),
                 "byte program rejected successful DQ7 completion");
    require_writes(rom, {
        {0xAAA, 0xAA}, {0x555, 0x55}, {0xAAA, 0xA0}, {0x123456, 0xA5},
    }, "byte program did not use the IS29GL byte-mode AAA/555 unlock sequence");
    test_require(rom.service_count == 1,
                 "byte program did not service USB while data polling");
}

static void test_sector_erase_sequence() {
    FakeRom rom;
    auto flash = programmer(rom);
    probe(flash, rom, 24);
    rom.reads = {0x7F, 0xFF};
    test_require(flash.erase_sector(0x12ABCD),
                 "sector erase rejected successful DQ7 completion");
    require_writes(rom, {
        {0xAAA, 0xAA}, {0x555, 0x55}, {0xAAA, 0x80},
        {0xAAA, 0xAA}, {0x555, 0x55}, {0x120000, 0x30},
    }, "sector erase command sequence or 128 KiB sector alignment is wrong");
}

static void test_chip_erase_sequence() {
    FakeRom rom;
    auto flash = programmer(rom);
    probe(flash, rom, 24);
    rom.reads = {0xFF};
    test_require(flash.erase_chip(), "chip erase did not complete");
    require_writes(rom, {
        {0xAAA, 0xAA}, {0x555, 0x55}, {0xAAA, 0x80},
        {0xAAA, 0xAA}, {0x555, 0x55}, {0xAAA, 0x10},
    }, "chip erase did not use the six-cycle IS29GL command");
}

static void test_dq5_failure_resets_device() {
    FakeRom rom;
    auto flash = programmer(rom);
    probe(flash, rom, 24);
    rom.reads = {0x20, 0x20};
    test_require(!flash.program_byte(0x42, 0x80),
                 "DQ5 timeout was treated as a successful program");
    test_require(!rom.writes.empty() && rom.writes.back() == std::make_pair(0u, uint8_t{0xF0}),
                 "failed operation did not reset the flash read array");
}

static void test_supported_capacities() {
    for (uint8_t power = 20; power <= 24; ++power) {
        FakeRom rom;
        auto flash = programmer(rom);
        probe(flash, rom, power);
    }

    FakeRom rom;
    auto flash = programmer(rom);
    rom.reads = {'Q', 'R', 'Y', 2, 0, 19, 1, 3, 0, 0, 2};
    test_require(!flash.probe() && !flash.capacity(), "unsupported sub-8-Mbit CFI device accepted");
}

int main() {
    test_byte_program_sequence();
    test_sector_erase_sequence();
    test_chip_erase_sequence();
    test_dq5_failure_resets_device();
    test_supported_capacities();
    std::puts("parallel_rom_programmer_tests: PASS");
    return 0;
}
