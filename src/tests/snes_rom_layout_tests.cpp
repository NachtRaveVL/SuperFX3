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
    std::puts("snes_rom_layout_tests: PASS");
    return 0;
}
