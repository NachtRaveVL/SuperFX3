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

static void require_writes(const FakeRom& rom,
                           std::initializer_list<std::pair<uint32_t, uint8_t>> expected,
                           const char* message) {
    test_require(rom.writes == std::vector<std::pair<uint32_t, uint8_t>>(expected), message);
}

static void test_byte_program_sequence() {
    FakeRom rom;
    rom.reads = {0x05, 0xA5, 0xA5};
    test_require(programmer(rom).program_byte(0x123456, 0xA5),
                 "byte program rejected successful DQ7 completion");
    require_writes(rom, {
        {0xAAA, 0xAA}, {0x555, 0x55}, {0xAAA, 0xA0}, {0x123456, 0xA5},
    }, "byte program did not use the IS29GL byte-mode AAA/555 unlock sequence");
    test_require(rom.service_count == 1,
                 "byte program did not service USB while data polling");
}

static void test_sector_erase_sequence() {
    FakeRom rom;
    rom.reads = {0x7F, 0xFF};
    test_require(programmer(rom).erase_sector(0x12ABCD),
                 "sector erase rejected successful DQ7 completion");
    require_writes(rom, {
        {0xAAA, 0xAA}, {0x555, 0x55}, {0xAAA, 0x80},
        {0xAAA, 0xAA}, {0x555, 0x55}, {0x120000, 0x30},
    }, "sector erase command sequence or 128 KiB sector alignment is wrong");
}

static void test_chip_erase_sequence() {
    FakeRom rom;
    rom.reads = {0xFF};
    test_require(programmer(rom).erase_chip(), "chip erase did not complete");
    require_writes(rom, {
        {0xAAA, 0xAA}, {0x555, 0x55}, {0xAAA, 0x80},
        {0xAAA, 0xAA}, {0x555, 0x55}, {0xAAA, 0x10},
    }, "chip erase did not use the six-cycle IS29GL command");
}

static void test_dq5_failure_resets_device() {
    FakeRom rom;
    rom.reads = {0x20, 0x20};
    test_require(!programmer(rom).program_byte(0x42, 0x80),
                 "DQ5 timeout was treated as a successful program");
    test_require(!rom.writes.empty() && rom.writes.back() == std::make_pair(0u, uint8_t{0xF0}),
                 "failed operation did not reset the flash read array");
}

static void test_supported_id() {
    FakeRom rom;
    rom.reads = {0x9D, 0x7E, 0x21, 0x01};
    test_require(programmer(rom).is_supported_device(),
                 "IS29GL128 JEDEC ID was not recognized");
    require_writes(rom, {
        {0xAAA, 0xAA}, {0x555, 0x55}, {0xAAA, 0x90}, {0, 0xF0},
    }, "autoselect did not use byte-mode ID sequence");
}

int main() {
    test_byte_program_sequence();
    test_sector_erase_sequence();
    test_chip_erase_sequence();
    test_dq5_failure_resets_device();
    test_supported_id();
    std::puts("parallel_rom_programmer_tests: PASS");
    return 0;
}
