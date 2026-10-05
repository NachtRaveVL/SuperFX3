#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../storage/snes_rom_layout.h"
#include "test_support.h"

static uint8_t read_vector(void* context, uint32_t offset) {
    const auto& image = *static_cast<const std::vector<uint8_t>*>(context);
    return offset < image.size() ? image[offset] : 0xFF;
}

static void put16(std::vector<uint8_t>& image, uint32_t offset, uint16_t value) {
    image[offset] = static_cast<uint8_t>(value);
    image[offset + 1] = static_cast<uint8_t>(value >> 8);
}

static void add_header(std::vector<uint8_t>& image, uint32_t offset, uint8_t mode) {
    const char title[] = "SUPERFX3 USB TEST";
    for (unsigned index = 0; index < sizeof(title) - 1; ++index)
        image[offset + index] = static_cast<uint8_t>(title[index]);
    for (unsigned index = sizeof(title) - 1; index < 21; ++index)
        image[offset + index] = ' ';
    image[offset + 0x15] = mode;
    put16(image, offset + 0x1C, 0xEDCB);
    put16(image, offset + 0x1E, 0x1234);
    put16(image, offset + 0x3C, 0x8000);
}

static void test_lorom_detection_and_mapping() {
    std::vector<uint8_t> image(1024u * 1024u, 0xFF);
    add_header(image, 0x7FC0, 0x20);
    SnesRomInfo info{};
    test_require(snes_rom_detect({&image, read_vector}, static_cast<uint32_t>(image.size()), false, info),
                 "valid LoROM header was not detected");
    test_require(info.map == SnesRomMap::LoRom && info.data_offset == 0,
                 "LoROM metadata was wrong");
    uint32_t source = 0;
    test_require(snes_rom_source_offset(info, 0x008000, source) && source == 0,
                 "LoROM $00:8000 did not map to source offset zero");
    test_require(!snes_rom_source_offset(info, 0x7E8000, source),
                 "SNES work RAM bank was treated as ROM");
}

static void test_headered_hirom_detection() {
    std::vector<uint8_t> image(2u * 1024u * 1024u + 512u, 0xFF);
    add_header(image, 512u + 0xFFC0u, 0x21);
    SnesRomInfo info{};
    test_require(snes_rom_detect({&image, read_vector}, static_cast<uint32_t>(image.size()), true, info),
                 "headered .smc HiROM was not detected");
    test_require(info.map == SnesRomMap::HiRom && info.data_offset == 512,
                 "HiROM copier-header metadata was wrong");
    uint32_t source = 0;
    test_require(snes_rom_source_offset(info, 0xC01234, source) && source == 0x1234,
                 "HiROM bank mapping was wrong");
}

static void test_header_detection_ignores_extension() {
    std::vector<uint8_t> headered(512u * 1024u + 512u, 0xFF);
    add_header(headered, 512u + 0x7FC0u, 0x20);
    SnesRomInfo info{};
    test_require(snes_rom_detect({&headered, read_vector},
                                 static_cast<uint32_t>(headered.size()), false, info) &&
                     info.data_offset == 512,
                 "512-byte copier header was not stripped from a mislabeled .sfc");

    std::vector<uint8_t> unheadered(512u * 1024u, 0xFF);
    add_header(unheadered, 0x7FC0u, 0x20);
    test_require(snes_rom_detect({&unheadered, read_vector},
                                 static_cast<uint32_t>(unheadered.size()), true, info) &&
                     info.data_offset == 0,
                 "an unheadered .smc was shifted based only on its extension");

    std::vector<uint8_t> fake_header(512u * 1024u + 512u, 0xA5);
    test_require(!snes_rom_detect({&fake_header, read_vector},
                                  static_cast<uint32_t>(fake_header.size()), true, info),
                 "a 512-byte size remainder was mistaken for a copier header");
}

static void test_extended_maps() {
    std::vector<uint8_t> exlo(6u * 1024u * 1024u, 0xFF);
    add_header(exlo, 0x7FC0u, 0x32);
    SnesRomInfo info{};
    test_require(snes_rom_detect({&exlo, read_vector},
                                 static_cast<uint32_t>(exlo.size()), false, info) &&
                     info.map == SnesRomMap::ExLoRom,
                 "6 MiB ExLoROM was not detected");
    uint32_t source = 0;
    test_require(snes_rom_source_offset(info, 0x008000u, source) && source == 0x400000u,
                 "ExLoROM lower bank did not select the upper source half");
    test_require(snes_rom_source_offset(info, 0x808000u, source) && source == 0,
                 "ExLoROM upper bank did not select the lower source half");

    std::vector<uint8_t> exhi(8u * 1024u * 1024u + 512u, 0xFF);
    add_header(exhi, 512u + 0x40FFC0u, 0x35);
    test_require(snes_rom_detect({&exhi, read_vector},
                                 static_cast<uint32_t>(exhi.size()), true, info) &&
                     info.map == SnesRomMap::ExHiRom && info.data_offset == 512u,
                 "headered 8 MiB ExHiROM was not detected");
    test_require(snes_rom_source_offset(info, 0x008000u, source) && source == 0x408000u,
                 "ExHiROM low bank did not select the upper source half");
    test_require(snes_rom_source_offset(info, 0xC01234u, source) && source == 0x1234u,
                 "ExHiROM high bank did not select the lower source half");
}

static void test_rejects_non_rom() {
    std::vector<uint8_t> image(128u * 1024u, 0);
    SnesRomInfo info{};
    test_require(!snes_rom_detect({&image, read_vector}, static_cast<uint32_t>(image.size()), false, info),
                 "non-ROM payload passed header validation");
}

static void test_fx3() {
    std::vector<uint8_t> rom(0x400000u);
    for (uint32_t i = 0; i < rom.size(); ++i)
        rom[i] = static_cast<uint8_t>((i >> 12) ^ (i >> 19) ^ i);
    add_header(rom, 0x7FC0u, 0x20);
    rom[0x7FBD] = 5;
    rom[0x7FD7] = 0x0C;
    SnesRomInfo info{};
    for (uint32_t type : {0x17u, 0x18u}) {
        rom[0x7FD6] = static_cast<uint8_t>(type);
        test_require(snes_rom_detect({&rom, read_vector}, 0x400000u, false, info) &&
                         info.map == SnesRomMap::Fx3 && info.size == 0x400000u &&
                         !info.data_offset && info.ram_size == 32u * 1024u &&
                         snes_rom_has_persistent_ram(info) == (type == 0x18u),
                     "canonical FX3 was misdetected as an ordinary ROM");
    }
    for (uint32_t address = 0; address < 0x1000000u; address += 0x1000u) {
        uint32_t source = 0;
        const uint32_t bank = address >> 16;
        if (bank == 0x7Eu || bank == 0x7Fu) {
            test_require(!snes_rom_source_offset(info, address, source), "FX3 mapped WRAM");
            continue;
        }
        const uint32_t b = bank & 0x7Fu;
        const uint32_t expected = b < 0x40u ?
            b * 0x8000u + (address & 0x7FFFu) : (b - 0x40u) * 0x10000u + (address & 0xFFFFu);
        test_require(snes_rom_source_offset(info, address, source) && source == expected,
                     "FX3 physical bank mapping changed");
    }
    std::vector<uint8_t> dump(0x800000u + 256u, 0xFF);
    for (uint32_t i = 0; i < 0x400000u; ++i) {
        dump[i] = rom[(i >> 16) * 0x8000u + (i & 0x7FFFu)];
        dump[0x400000u + i] = rom[i];
    }
    for (uint32_t size : {0x800000u, 0x800100u}) {
        test_require(snes_rom_detect({&dump, read_vector}, size, false, info) &&
                         info.map == SnesRomMap::Fx3 && info.data_offset == 0x400000u &&
                         info.size == 0x400000u, "production FX3 dump/trailer was not stripped");
    }
    std::vector<uint8_t> headered_dump(512u + dump.size(), 0);
    std::copy(dump.begin(), dump.end(), headered_dump.begin() + 512u);
    test_require(snes_rom_detect({&headered_dump, read_vector},
                                 static_cast<uint32_t>(headered_dump.size()), false, info) &&
                     info.map == SnesRomMap::Fx3 && info.data_offset == 0x400200u &&
                     info.size == 0x400000u,
                 "content-valid copier header plus FX3 erased trailer was not detected");
    for (uint32_t offset : {0u, 0x8000u, 0x3FFFFFu, 0x800000u}) {
        dump[offset] ^= 1;
        test_require(!snes_rom_detect({&dump, read_vector}, 0x800100u, false, info),
                     "malformed FX3 dump accepted");
        dump[offset] ^= 1;
    }
    std::fill(dump.begin(), dump.end(), 0);
    test_require(!snes_rom_detect({&dump, read_vector}, 0x800000u, false, info) ||
                     (info.map != SnesRomMap::Fx3 && info.size == 0x800000u),
                 "arbitrary 8 MiB image stripped as FX3");
    snes_rom_descriptor({SnesRomMap::Fx3, 0x400000u, 0, 32u * 1024u, 0x17u},
                        dump.data() + SNES_ROM_DESCRIPTOR_ADDRESS);
    const SnesRomInfo installed = snes_rom_installed_info({&dump, read_vector});
    test_require(installed.map == SnesRomMap::Fx3 && installed.ram_size == 32u * 1024u &&
                     installed.cartridge_type == 0x17u &&
                     !snes_rom_has_persistent_ram(installed),
                 "FX3 descriptor lost SRAM/type metadata");
    dump[SNES_ROM_DESCRIPTOR_ADDRESS + 12u] ^= 1;
    test_require(snes_rom_installed_map({&dump, read_vector}) == SnesRomMap::Fx3Physical,
                 "corrupt descriptor selected canonical FX3");
    for (uint32_t size : {0x100000u, 0x200000u, 0x300000u}) {
        rom.resize(size);
        rom[0x7FD7] = size == 0x100000u ? 0x0A : size == 0x200000u ? 0x0B : 0x0C;
        test_require(snes_rom_detect({&rom, read_vector}, size, false, info) &&
                         info.map == SnesRomMap::Fx3 && info.size == size,
                     "smaller canonical FX3 not detected");
        uint32_t source = 0;
        test_require(snes_rom_source_offset(info, 0x600000u, source) &&
                         source == (size <= 0x200000u ? 0u : 0x200000u),
                     "FX3 third MiB did not mirror smaller ROM");
        snes_rom_descriptor(info, dump.data() + SNES_ROM_DESCRIPTOR_ADDRESS);
        test_require(snes_rom_installed_map({&dump, read_vector}) == SnesRomMap::Fx3,
                     "smaller FX3 descriptor rejected");
    }
}

int main() {
    for (uint32_t size : {32768u, 65536u}) {
        for (uint32_t header : {0u, 512u}) {
            std::vector<uint8_t> small(size + header, 0xFF);
            add_header(small, header + 0x7FC0u, 0x20);
            SnesRomInfo info{};
            test_require(snes_rom_detect({&small, read_vector}, size + header, false, info) &&
                             info.size == size && info.data_offset == header,
                         "valid small/headered LoROM rejected");
        }
    }
    std::vector<uint8_t> physical(16u * 1024u * 1024u, 0xFF);
    for (auto map : {SnesRomMap::ExLoRom, SnesRomMap::ExHiRom}) {
        snes_rom_descriptor({map, 8u * 1024u * 1024u, 0},
                             physical.data() + SNES_ROM_DESCRIPTOR_ADDRESS);
        test_require(snes_rom_installed_map({&physical, read_vector}) == map,
                     "installed map did not survive descriptor round trip");
        physical[SNES_ROM_DESCRIPTOR_ADDRESS] ^= 1;
        test_require(snes_rom_installed_map({&physical, read_vector}) == SnesRomMap::Fx3Physical,
                     "corrupt descriptor accepted");
    }
    test_lorom_detection_and_mapping();
    test_headered_hirom_detection();
    test_header_detection_ignores_extension();
    test_extended_maps();
    test_rejects_non_rom();
    test_fx3();
    std::puts("snes_rom_layout_tests: PASS");
    return 0;
}
