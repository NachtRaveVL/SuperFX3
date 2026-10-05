#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "test_support.h"
#include "../platform/rp2350/qspi_sd.h"
#include "../platform/rp2350/fx3_audio_sd.h"
#include "../platform/rp2350/sd_audio_source.h"

namespace {
using Sector = std::array<uint8_t, 512>;
std::map<uint32_t, Sector> disk;
std::vector<uint8_t> expected(1305);
bool version2 = true, present = true, bad_csd = false;
bool corrupt = false, busy = false;
bool bad_echo = false, app_command = false;
unsigned idle_attempts = 0;
unsigned guest_services = 0;
uint32_t corrupt_sector = UINT32_MAX, error_sector = UINT32_MAX;
uint32_t file_first = 0, fat_first = 0;
unsigned block_length_commands = 0;

void put16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
}
void put32(uint8_t* data, uint32_t value) {
    put16(data, static_cast<uint16_t>(value));
    put16(data + 2, static_cast<uint16_t>(value >> 16));
}
uint16_t crc16(const uint8_t* data, uint32_t size) {
    uint32_t crc = 0;
    for (uint32_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = ((crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0u)) & 0xFFFFu;
    }
    return static_cast<uint16_t>(crc);
}
void finish_crc(uint8_t* data, uint32_t size) {
    const uint16_t crc = crc16(data, size);
    data[size] = static_cast<uint8_t>(crc >> 8);
    data[size + 1u] = static_cast<uint8_t>(crc);
}
void entry(uint8_t* data, const char* name, uint8_t attributes, uint32_t cluster, uint32_t size) {
    std::memcpy(data, name, 11);
    data[11] = attributes;
    put16(data + 20, static_cast<uint16_t>(cluster >> 16));
    put16(data + 26, static_cast<uint16_t>(cluster));
    put32(data + 28, size);
}
void make_disk(bool fat32, bool partitioned) {
    disk.clear();
    present = true;
    corrupt = busy = bad_csd = false;
    bad_echo = app_command = false;
    idle_attempts = 2;
    corrupt_sector = error_sector = UINT32_MAX;
    const uint32_t base = partitioned ? 2048u : 0;
    if (partitioned) {
        Sector& mbr = disk[0];
        mbr[510] = 0x55; mbr[511] = 0xAA;
        mbr[450] = fat32 ? 0x0C : 0x06;
        put32(mbr.data() + 454, base);
        put32(mbr.data() + 458, fat32 ? 80000u : 10000u);
    }
    Sector& boot = disk[base];
    boot[0] = 0xEB; boot[2] = 0x90;
    boot[510] = 0x55; boot[511] = 0xAA;
    put16(boot.data() + 11, 512);
    boot[13] = 1;
    put16(boot.data() + 14, fat32 ? 32 : 1);
    boot[16] = fat32 ? 2 : 1;
    if (fat32) {
        put32(boot.data() + 32, 80000);
        put32(boot.data() + 36, 620);
        put16(boot.data() + 40, 0x81); // Only FAT 1 is active.
        put32(boot.data() + 44, 2);
    } else {
        put16(boot.data() + 17, 16);
        put16(boot.data() + 19, 10000);
        put16(boot.data() + 22, 40);
    }
    const uint32_t data = base + (fat32 ? 1272u : 42u);
    const uint32_t root = fat32 ? data : base + 41u;
    const uint32_t audio = fat32 ? 5u : 2u;
    entry(disk[root].data(), "AUDIO      ", 0x10, audio, 0);
    entry(disk[data + audio - 2u].data(), "1234    BRR", 0x20, 3, static_cast<uint32_t>(expected.size()));
    entry(disk[data + audio - 2u].data() + 32, "0000    BRR", 0x20, 0, 0);
    fat_first = base + (fat32 ? 652u : 1u);
    auto set_fat = [&](uint32_t cluster, uint32_t next) {
        uint8_t* at = disk[fat_first].data() + cluster * (fat32 ? 4u : 2u);
        if (fat32) put32(at, next); else put16(at, static_cast<uint16_t>(next));
    };
    set_fat(3, 10); set_fat(10, 7); set_fat(7, fat32 ? 0x0FFFFFFFu : 0xFFFFu);
    file_first = data + 1u;
    const uint32_t clusters[3] {3, 10, 7};
    for (uint32_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<uint8_t>((i * 17u) ^ (i >> 4));
        if (!(i % 9u)) expected[i] &= 0xFCu;
        disk[data + clusters[i / 512u] - 2u][i % 512u] = expected[i];
    }
}

void check_stream(bool fat32, bool partitioned, bool v2) {
    version2 = v2;
    make_disk(fat32, partitioned);
    const Fx3AudioSource source = fx3_audio_sd_source();
    test_require(source.open(source.context, 0x1234), "real SD/FAT source failed to open the asset");
    uint8_t output[4096] {};
    uint32_t used = 0;
    for (uint32_t request : {1u, 510u, 3u, 700u, 4096u}) {
        const int32_t read = source.read(source.context, output + used, request);
        test_require(read >= 0, "fragmented file read failed");
        used += static_cast<uint32_t>(read);
    }
    test_require(used == expected.size() && !std::memcmp(output, expected.data(), used) &&
                     source.read(source.context, output, 1) == 0,
                 "SD/FAT source changed file bytes, truncation, or EOF");
    source.close(source.context);
    test_require(sd_audio_read(output, 1) == -1, "closed file remained readable");
    test_require(sd_audio_open("/audio/0000.brr") && sd_audio_read(output, 1) == 0,
                 "empty file was not handled as EOF");
    test_require(!sd_audio_open("/audio/FFFF.brr"), "missing asset was accepted");
}

void failures_and_transport() {
    version2 = true;
    make_disk(false, false);
    test_require(sd_audio_mount() && sd_audio_open("/audio/1234.brr"), "retry setup failed");
    uint8_t output[4096] {};
    busy = true;
    test_require(sd_audio_read(output, 512) == -2, "bus contention was not retryable");
    busy = false; corrupt = true; corrupt_sector = file_first;
    test_require(sd_audio_read(output, 512) == -2, "bad block CRC reached the FIFO");
    corrupt = false;
    test_require(sd_audio_read(output, 512) == 512 && !std::memcmp(output, expected.data(), 512),
                 "retry consumed or duplicated file bytes");
    error_sector = file_first + 7u;
    test_require(sd_audio_read(output, 512) == -1, "card read error was ignored");
    make_disk(false, false);
    test_require(sd_audio_mount() && sd_audio_open("/audio/1234.brr"), "CRC failure setup failed");
    corrupt = true; corrupt_sector = file_first;
    test_require(sd_audio_read(output, 1) == -2 && sd_audio_read(output, 1) == -2 &&
                     sd_audio_read(output, 1) == -1, "persistent CRC error retried indefinitely");
    make_disk(false, false);
    put16(disk[fat_first].data() + 6, 3);
    test_require(sd_audio_mount() && sd_audio_open("/audio/1234.brr") &&
                     sd_audio_read(output, 4096) == 512 && sd_audio_read(output, 1) == -1,
                 "corrupt FAT chain replayed source bytes");
    make_disk(false, false);
    disk[0][13] = 3;
    test_require(!sd_audio_mount(), "invalid BPB cluster geometry mounted");
    make_disk(false, true);
    put32(disk[0].data() + 454, 0xFFFFFFF0u);
    test_require(!sd_audio_mount(), "out-of-card partition mounted");
    make_disk(false, false);
    put16(disk[0].data() + 19, 2000); // Cluster count identifies unsupported FAT12.
    test_require(!sd_audio_mount(), "FAT12 was parsed as FAT16");
    make_disk(true, false);
    const uint32_t root = 1272, audio = root + 3u;
    disk[root + 2u] = disk[root];
    disk[root].fill(0xE5);
    put32(disk[fat_first].data() + 8, 4);
    disk[audio + 1u] = disk[audio];
    disk[audio].fill(0xE5);
    put32(disk[fat_first].data() + 20, 6);
    test_require(sd_audio_mount() && sd_audio_open("/audio/1234.brr") &&
                     sd_audio_read(output, 4096) == static_cast<int32_t>(expected.size()) &&
                     !std::memcmp(output, expected.data(), expected.size()),
                 "FAT32 root/audio directory chains were not followed");
    make_disk(false, false);
    put16(disk[fat_first].data() + 6, 0xFFFF);
    test_require(sd_audio_mount() && sd_audio_open("/audio/1234.brr") &&
                     sd_audio_read(output, 4096) == 512 && sd_audio_read(output, 1) == -1,
                 "premature FAT EOF silently truncated the declared file");
    make_disk(false, false);
    bad_csd = true;
    test_require(!sd_audio_mount(), "invalid CSD CRC was accepted");
    make_disk(false, false);
    bad_echo = true;
    test_require(!sd_audio_mount(), "invalid CMD8 voltage/echo was accepted");
    bad_csd = false; present = false;
    test_require(!sd_audio_mount(), "missing card mounted");
    fx3_audio_init(fx3_audio_sd_source());
    fx3_audio_host_write(fx3_audio::MMIO_BASE, static_cast<uint8_t>(Fx3AudioCommand::Play));
    fx3_audio_task();
    test_require(fx3_audio_host_read(fx3_audio::MMIO_BASE + fx3_audio::Status) ==
                     static_cast<uint8_t>(Fx3AudioStatus::Error), "missing card crashed or started playback");
    make_disk(false, false);
    fx3_audio_init(fx3_audio_sd_source());
    fx3_audio_host_write(fx3_audio::MMIO_BASE + fx3_audio::AssetIdLow, 0x34);
    fx3_audio_host_write(fx3_audio::MMIO_BASE + fx3_audio::AssetIdHigh, 0x12);
    fx3_audio_host_write(fx3_audio::MMIO_BASE, static_cast<uint8_t>(Fx3AudioCommand::Play));
    fx3_audio_host_write(fx3_audio::MMIO_BASE + fx3_audio::SpcLevel, 4);
    for (unsigned i = 0; i < 8; ++i) fx3_audio_task();
    const uint8_t page = fx3_audio_host_read(fx3_audio::MMIO_BASE + fx3_audio::HdmaPage);
    test_require(page < fx3_audio::HDMA_PAGE_COUNT, "SD/FAT data did not produce a READY HDMA page");
    fx3_audio_host_write(fx3_audio::MMIO_BASE + fx3_audio::HdmaPage, page);
    const uint16_t address = static_cast<uint16_t>(fx3_audio::HDMA_BASE + page * fx3_audio::HDMA_PAGE_SIZE);
    for (uint16_t i = 0; i < 3; ++i)
        test_require(fx3_audio_hdma_read(static_cast<uint16_t>(address + 1u + i)) == expected[i],
                     "SD/FAT audio bytes changed before HDMA delivery");
    fx3_audio_host_write(fx3_audio::MMIO_BASE, static_cast<uint8_t>(Fx3AudioCommand::Stop));
    fx3_audio_task();
}
}

QspiSdResult qspi_sd_exchange(const uint8_t*, uint8_t*, uint32_t size, uint32_t clock, bool select) {
    test_require(size == 10 && clock <= 400000 && !select, "SD startup clocks asserted CS or ran too fast");
    return present ? QspiSdResult::Ok : QspiSdResult::Timeout;
}
bool fx_sync_core1_service() { ++guest_services; return false; }
QspiSdResult qspi_sd_command(const uint8_t* packet, uint8_t* response, uint32_t response_size,
                            uint8_t* block, uint32_t block_size, uint32_t clock) {
    if (!present) return QspiSdResult::Timeout;
    const uint8_t command = packet[0] & 0x3Fu;
    const uint32_t argument = (static_cast<uint32_t>(packet[1]) << 24) |
        (static_cast<uint32_t>(packet[2]) << 16) | (static_cast<uint32_t>(packet[3]) << 8) | packet[4];
    std::memset(response, 0, response_size);
    switch (command) {
        case 0: test_require(packet[5] == 0x95, "CMD0 CRC7 is wrong"); response[0] = 1; break;
        case 8:
            test_require(packet[5] == 0x87 && clock <= 400000, "CMD8 CRC7/clock is wrong");
            response[0] = version2 ? 1 : 5; response[3] = 1; response[4] = bad_echo ? 0xAB : 0xAA; break;
        case 55: response[0] = 1; app_command = true; break;
        case 41:
            test_require(app_command && argument == (version2 ? 0x40000000u : 0), "ACMD41 prefix/HCS is wrong");
            app_command = false;
            if (idle_attempts) { --idle_attempts; response[0] = 1; }
            break;
        case 58: response[1] = version2 ? 0xC0 : 0x80; response[2] = 0xFF; break;
        case 16: test_require(!version2 && argument == 512, "SDSC block length was not selected"); ++block_length_commands; break;
        case 9:
            test_require(block_size == 16, "CSD block size is wrong");
            std::memset(block, 0, 18);
            if (version2) { block[0] = 0x40; block[9] = 127; }
            else { block[5] = 9; block[6] = 3; block[7] = 0xFF; block[8] = 0xC0; block[9] = 1; block[10] = 0x80; }
            finish_crc(block, 16);
            if (bad_csd) block[17] ^= 1;
            break;
        case 17: {
            test_require(block_size == 512, "file reader requested a non-sector block");
            const uint32_t lba = version2 ? argument : argument / 512u;
            if (!version2) test_require(argument % 512u == 0, "SDSC address was not byte-based");
            if (busy) return QspiSdResult::Busy;
            if (lba == error_sector) return QspiSdResult::ProtocolError;
            std::memcpy(block, disk[lba].data(), 512);
            finish_crc(block, 512);
            if (corrupt && lba == corrupt_sector) block[513] ^= 1;
            break;
        }
        default: test_require(false, "read-only source issued an unexpected SD command");
    }
    return QspiSdResult::Ok;
}

int main() {
    check_stream(false, false, true);
    check_stream(false, true, false);
    check_stream(true, true, true);
    test_require(block_length_commands != 0, "SDSC setup was not exercised");
    test_require(guest_services >= 6, "slow card initialization stopped all guest progress");
    failures_and_transport();
    // Simultaneous audio/video use independent FAT cursors and one sector cache.
    version2 = true;
    make_disk(false, false);
    entry(disk[41].data() + 32, "VIDEO      ", 0x10, 11, 0);
    entry(disk[51].data(), "1234    FMV", 0x20, 12, 512);
    put16(disk[1].data() + 22, 0xFFFF);
    put16(disk[1].data() + 24, 0xFFFF);
    disk[52].fill(0x5A);
    const Fx3AudioSource audio = fx3_audio_sd_source();
    test_require(audio.open(audio.context, 0x1234) && sd_video_mount() &&
                     sd_video_open("/video/1234.fmv"), "paired video/audio open failed");
    uint8_t output[512] {};
    test_require(audio.read(audio.context, output, 100) == 100 && output[99] == expected[99],
                 "audio cursor was reset by video mount/open");
    test_require(sd_video_read(output, 512) == 512 && output[0] == 0x5A && output[511] == 0x5A,
                 "video read used the audio FAT cursor/cache");
    test_require(audio.read(audio.context, output, 100) == 100 && output[0] == expected[100],
                 "video read advanced the audio position");
    sd_video_close();
    test_require(audio.read(audio.context, output, 100) == 100 && output[0] == expected[200],
                 "closing video also closed audio");
    audio.close(audio.context);
    std::puts("sd_audio_source_tests: PASS");
}
