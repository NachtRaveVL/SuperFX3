#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../storage/fx3_save_journal.h"
#include "test_support.h"

static_assert(fx3_save::PAYLOAD_SIZE == 128u * 1024u && fx3_save::HEADER_SIZE == 256u &&
              fx3_save::SLOT_SIZE == 132u * 1024u, "Save slot format must not change.");
static_assert(fx3_save::SLOT_COUNT == 4 && 4u * fx3_save::SLOT_SIZE == 528u * 1024u &&
              fx3_qspi::SAVE_OFFSET == 0x07C000u &&
              fx3_qspi::SAVE_OFFSET + 4u * fx3_save::SLOT_SIZE == 0x100000u,
              "Four snapshots must fit exactly before the unchanged FX-ROM partition.");

struct TestFlash {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(fx3_qspi::FLASH_SIZE, 0xFF);
    uint32_t erase_count = 0;
    uint32_t erased_offset = 0;
    uint32_t erased_size = 0;
    bool fail_program = false;
    bool fail_erase = false;
};

static bool erase_flash(void* context, uint32_t offset, uint32_t size) {
    auto& flash = *static_cast<TestFlash*>(context);
    if (flash.fail_erase ||
        (offset & (fx3_qspi::FLASH_SECTOR_SIZE - 1u)) ||
        (size & (fx3_qspi::FLASH_SECTOR_SIZE - 1u)) ||
        offset > flash.bytes.size() || size > flash.bytes.size() - offset) {
        return false;
    }

    std::fill(flash.bytes.begin() + offset, flash.bytes.begin() + offset + size, 0xFF);
    flash.erase_count++;
    flash.erased_offset = offset;
    flash.erased_size = size;
    return true;
}

static bool program_flash(void* context, uint32_t offset, const uint8_t* data, uint32_t size) {
    auto& flash = *static_cast<TestFlash*>(context);
    if (flash.fail_program || !data ||
        (offset & (fx3_qspi::FLASH_PAGE_SIZE - 1u)) ||
        (size & (fx3_qspi::FLASH_PAGE_SIZE - 1u)) ||
        offset > flash.bytes.size() || size > flash.bytes.size() - offset) {
        return false;
    }

    for (uint32_t i = 0; i < size; ++i) {
        if ((static_cast<uint8_t>(~flash.bytes[offset + i]) & data[i]) != 0)
            return false;
        flash.bytes[offset + i] &= data[i];
    }
    return true;
}

static fx3_save::QspiFlash make_flash(TestFlash& storage) {
    return fx3_save::QspiFlash{
        &storage, storage.bytes.data(), static_cast<uint32_t>(storage.bytes.size()),
        erase_flash, program_flash
    };
}

static std::vector<uint8_t> snapshot(uint8_t seed) {
    std::vector<uint8_t> data(fx3_save::PAYLOAD_SIZE);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(seed + i * 17u);
    return data;
}

static void test_empty_and_input_validation() {
    TestFlash storage;
    auto flash = make_flash(storage);
    std::vector<uint8_t> output(fx3_save::PAYLOAD_SIZE);

    test_require(!fx3_save::scan(flash).valid, "erased journal reported a valid record");
    test_require(!fx3_save::restore(flash, output.data(), output.size()),
                 "erased journal restored nonexistent data");
    test_require(!fx3_save::append(flash, output.data(), output.size() - 1),
                 "journal accepted the wrong snapshot size");
    test_require(fx3_save::crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u,
                 "journal CRC-32 does not match the standard check vector");
}

static void test_append_restore_and_crc_fallback() {
    TestFlash storage;
    auto flash = make_flash(storage);
    const auto first = snapshot(0x11);
    const auto second = snapshot(0x82);
    std::vector<uint8_t> output(fx3_save::PAYLOAD_SIZE);
    fx3_save::Record record{};

    test_require(fx3_save::append(flash, first.data(), first.size(), &record),
                 "first journal append failed");
    test_require(record.valid && record.sequence == 0 && record.slot == 0,
                 "first journal record metadata is wrong");
    test_require(fx3_save::append(flash, second.data(), second.size(), &record),
                 "second journal append failed");
    test_require(record.sequence == 1 && record.slot == 1,
                 "second journal record metadata is wrong");

    test_require(fx3_save::restore(flash, output.data(), output.size(), &record),
                 "latest journal restore failed");
    test_require(output == second && record.sequence == 1,
                 "journal did not restore the newest record");

    const uint32_t second_payload = fx3_qspi::SAVE_OFFSET + fx3_save::SLOT_SIZE +
                                    fx3_save::HEADER_SIZE;
    storage.bytes[second_payload + 123] ^= 0x01;
    test_require(fx3_save::restore(flash, output.data(), output.size(), &record),
                 "journal did not fall back after a CRC failure");
    test_require(output == first && record.sequence == 0 && record.slot == 0,
                 "journal CRC fallback selected the wrong record");
}

static void test_interrupted_slot_and_wrap_erase() {
    TestFlash storage;
    auto flash = make_flash(storage);
    const auto data0 = snapshot(0x20);
    const auto data1 = snapshot(0x40);
    const auto data2 = snapshot(0x60);
    const auto data3 = snapshot(0x80);
    const auto data4 = snapshot(0xA0);
    fx3_save::Record record{};

    test_require(fx3_save::append(flash, data0.data(), data0.size(), &record),
                 "initial append failed");

    // Model power loss after payload programming but before the commit header.
    const uint32_t dirty_payload = fx3_qspi::SAVE_OFFSET + fx3_save::SLOT_SIZE +
                                   fx3_save::HEADER_SIZE;
    test_require(program_flash(&storage, dirty_payload, data1.data(),
                               static_cast<uint32_t>(data1.size())),
                 "failed to construct interrupted journal slot");

    test_require(fx3_save::append(flash, data2.data(), data2.size(), &record),
                 "append after interrupted slot failed");
    test_require(record.slot == 2 && record.sequence == 1,
                 "journal reused a dirty uncommitted slot");

    test_require(fx3_save::append(flash, data3.data(), data3.size(), &record),
                 "journal wrap append failed");
    test_require(storage.erase_count == 0 && record.slot == 3 && record.sequence == 2,
                 "journal erased before consuming the fourth slot");
    test_require(fx3_save::append(flash, data4.data(), data4.size(), &record),
                 "journal wrap append failed");
    test_require(storage.erase_count == 1 && record.slot == 0 && record.sequence == 3,
                 "journal did not erase exactly once on wrap");

    std::vector<uint8_t> output(fx3_save::PAYLOAD_SIZE);
    test_require(fx3_save::restore(flash, output.data(), output.size(), &record) && output == data4,
                 "journal did not restore the post-wrap record");
}

static void test_four_snapshots_and_destination_only_wrap() {
    TestFlash storage;
    auto flash = make_flash(storage);
    fx3_save::Record record{};
    std::vector<uint8_t> output(fx3_save::PAYLOAD_SIZE);
    // Non-erased sentinels protect both adjacent partitions.
    std::fill(storage.bytes.begin(), storage.bytes.begin() + fx3_qspi::SAVE_OFFSET, 0xA5);
    std::fill(storage.bytes.begin() + fx3_qspi::FX_CODE_OFFSET, storage.bytes.end(), 0x5A);
    for (uint32_t i = 0; i < 4; ++i) {
        const auto data = snapshot(static_cast<uint8_t>(i));
        test_require(fx3_save::append(flash, data.data(), data.size(), &record), "four-slot fill failed");
        test_require(record.slot == i && record.sequence == i && storage.erase_count == 0,
                     "four snapshots did not fit without erasing");
        test_require(fx3_save::restore(flash, output.data(), output.size()) && output == data,
                     "restore did not select the newest of four snapshots");
    }
    const auto before = storage.bytes;
    const auto next = snapshot(4);
    test_require(fx3_save::append(flash, next.data(), next.size(), &record), "fifth append failed");
    test_require(record.slot == 0 && record.sequence == 4 && storage.erase_count == 1 &&
                     storage.erased_offset == fx3_qspi::SAVE_OFFSET &&
                     storage.erased_size == fx3_save::SLOT_SIZE,
                 "wrap did not erase only the destination slot");
    test_require(std::equal(before.begin(), before.begin() + fx3_qspi::SAVE_OFFSET,
                            storage.bytes.begin()) &&
                     std::equal(before.begin() + fx3_qspi::SAVE_OFFSET + fx3_save::SLOT_SIZE,
                                before.end(),
                                storage.bytes.begin() + fx3_qspi::SAVE_OFFSET + fx3_save::SLOT_SIZE),
                 "wrap modified another slot, firmware, or FX ROM");
    test_require(fx3_save::restore(flash, output.data(), output.size()) && output == next,
                 "restore did not select the post-wrap snapshot");
}

static void test_callback_failure() {
    TestFlash storage;
    auto flash = make_flash(storage);
    const auto data = snapshot(0xA0);
    storage.fail_program = true;

    test_require(!fx3_save::append(flash, data.data(), data.size()),
                 "journal ignored a flash-program failure");
    test_require(!fx3_save::scan(flash).valid,
                 "failed program created a committed journal record");

    storage.fail_program = false;
    std::fill(storage.bytes.begin() + fx3_qspi::SAVE_OFFSET,
              storage.bytes.begin() + fx3_qspi::SAVE_OFFSET + fx3_qspi::SAVE_SIZE, 0x00);
    storage.fail_erase = true;
    test_require(!fx3_save::append(flash, data.data(), data.size()),
                 "journal ignored a flash-erase failure");
}

static void test_failed_wrap_preserves_latest() {
    TestFlash storage;
    auto flash = make_flash(storage);
    const auto data = snapshot(0x5A);
    for (uint32_t i = 0; i < fx3_save::SLOT_COUNT; ++i)
        test_require(fx3_save::append(flash, data.data(), data.size()), "fill failed");
    test_require(storage.bytes[fx3_qspi::SAVE_OFFSET + 0] == 'S' &&
                     storage.bytes[fx3_qspi::SAVE_OFFSET + 1] == 'F' &&
                     storage.bytes[fx3_qspi::SAVE_OFFSET + 2] == 'X' &&
                     storage.bytes[fx3_qspi::SAVE_OFFSET + 3] == '3', "SFX3 byte order wrong");
    const auto previous = fx3_save::scan(flash);
    storage.fail_program = true;
    test_require(!fx3_save::append(flash, data.data(), data.size()), "failed wrap accepted");
    const auto retained = fx3_save::scan(flash);
    test_require(retained.valid && retained.slot == previous.slot &&
                     retained.sequence == previous.sequence,
                 "failed wrap destroyed the last committed save");
    storage.fail_program = false;
    test_require(fx3_save::append(flash, data.data(), data.size()), "retry after failed wrap failed");
    // Only the finalized SFX3 signature is valid. Reject the earlier misspelling.
    const auto latest = fx3_save::scan(flash);
    const uint32_t address = fx3_qspi::SAVE_OFFSET + latest.slot * fx3_save::SLOT_SIZE;
    storage.bytes[address + 1] = 'X';
    storage.bytes[address + 2] = 'F';
    test_require(fx3_save::scan(flash).sequence != latest.sequence, "misspelled signature accepted");
}

int main() {
    test_empty_and_input_validation();
    test_append_restore_and_crc_fallback();
    test_interrupted_slot_and_wrap_erase();
    test_four_snapshots_and_destination_only_wrap();
    test_callback_failure();
    test_failed_wrap_preserves_latest();
    std::puts("save_journal_tests: PASS");
    return 0;
}
