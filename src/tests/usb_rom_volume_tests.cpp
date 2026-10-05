#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../storage/usb_rom_volume.h"
#include "test_support.h"

struct Sink {
    bool begun = false;
    UsbRomFileType type = UsbRomFileType::Sfc;
    uint32_t completed = 0;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(4096, 0xFF);
};

static bool begin(void* context, UsbRomFileType type, uint32_t cluster) {
    auto& sink = *static_cast<Sink*>(context);
    sink.begun = cluster == 3;
    sink.type = type;
    return sink.begun;
}
static bool write(void* context, uint32_t offset, const uint8_t* data, size_t size) {
    auto& sink = *static_cast<Sink*>(context);
    if (offset + size > sink.bytes.size()) return false;
    std::memcpy(sink.bytes.data() + offset, data, size);
    return true;
}
static uint8_t read(void* context, uint32_t offset) {
    auto& sink = *static_cast<Sink*>(context);
    return offset < sink.bytes.size() ? sink.bytes[offset] : 0xFF;
}
static bool complete(void* context, UsbRomFileType type, uint32_t size,
                     const uint16_t*, uint32_t) {
    static_cast<Sink*>(context)->type = type;
    static_cast<Sink*>(context)->completed = size;
    return true;
}
static bool flush(void*) { return true; }

static void put16(uint8_t* data, unsigned offset, uint16_t value) {
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1] = static_cast<uint8_t>(value >> 8);
}
static void put32(uint8_t* data, unsigned offset, uint32_t value) {
    put16(data, offset, static_cast<uint16_t>(value));
    put16(data, offset + 2, static_cast<uint16_t>(value >> 16));
}

int main() {
    Sink sink;
    UsbRomVolume volume({&sink, begin, write, read, complete, flush});
    std::array<uint8_t, 512> block{};
    test_require(volume.read(0, block.data(), block.size()) && block[510] == 0x55 && block[511] == 0xAA,
                 "virtual FAT16 boot sector is invalid");
    volume.set_capacity(1024u * 1024u);
    test_require(volume.read(0, block.data(), block.size()), "resized boot sector was unreadable");
    const uint32_t total_blocks = static_cast<uint32_t>(block[32]) |
        (static_cast<uint32_t>(block[33]) << 8) |
        (static_cast<uint32_t>(block[34]) << 16) |
        (static_cast<uint32_t>(block[35]) << 24);
    test_require(volume.block_count() == UsbRomVolume::FIRST_UPLOAD_BLOCK +
                         (1024u * 1024u / UsbRomVolume::PAGE_SIZE + 4u) * 8u &&
                     total_blocks == volume.block_count() &&
                     std::memcmp(block.data() + 54, "FAT12   ", 8) == 0,
                 "8-Mbit device did not resize the advertised FAT volume");
    test_require(volume.read(1, block.data(), block.size()), "FAT12 table was unreadable");
    block[4] = static_cast<uint8_t>((block[4] & 0x0Fu) | 0xF0u);
    block[5] = 0xFF;
    test_require(volume.write(1, block.data(), block.size()), "FAT12 entry write failed");
    block.fill(0);
    std::memcpy(block.data(), "SMALL   SFC", 11);
    block[11] = 0x20;
    put16(block.data(), 26, 3);
    put32(block.data(), 28, 512);
    test_require(volume.write(65, block.data(), block.size()), "FAT12 root write failed");
    block.fill(0x3C);
    test_require(volume.write(89, block.data(), block.size()) && volume.eject() &&
                     sink.completed == 512,
                 "FAT12 upload chain did not complete");
    volume.set_capacity(UsbRomVolume::MAX_CAPACITY);
    sink.begun = false;
    sink.completed = 0;

    // FAT cluster 3 is the one-cluster upload chain.
    block.fill(0);
    put16(block.data(), 6, 0xFFFF);
    test_require(volume.write(1, block.data(), block.size()), "FAT write failed");

    block.fill(0);
    std::memcpy(block.data(), "GAME    SFC", 11);
    block[11] = 0x20;
    put16(block.data(), 26, 3);
    put32(block.data(), 28, 512);
    test_require(volume.write(65, block.data(), block.size()), "root write failed");
    test_require(!sink.begun, "metadata alone started programming");

    block.fill(0xA5);
    test_require(volume.write(89, block.data(), block.size()), "data write failed");
    test_require(!sink.completed && volume.eject(), "upload finalized before eject");
    test_require(sink.completed == 512 && sink.bytes[0] == 0xA5,
                 "completed ROM upload was not delivered to the sink");

    block.fill(0);
    test_require(volume.read(89, block.data(), block.size()) && block[0] == 0xA5,
                 "uploaded data did not read back through MSC");

    // Raw .rom/.bin entries bypass SNES layout detection and represent the
    // complete 16 MiB physical bus image.
    volume.reset();
    sink.begun = false;
    sink.completed = 0;
    block.fill(0);
    put16(block.data(), 6, 0xFFFF);
    test_require(volume.write(1, block.data(), block.size()), "raw FAT write failed");
    block.fill(0);
    std::memcpy(block.data(), "IMAGE   ROM", 11);
    block[11] = 0x20;
    put16(block.data(), 26, 3);
    put32(block.data(), 28, 512);
    test_require(volume.write(65, block.data(), block.size()), "raw root write failed");
    block.fill(0xA5);
    test_require(volume.write(89, block.data(), block.size()) && volume.eject() &&
                     sink.type == UsbRomFileType::Raw,
                 "ROM eject did not select the raw physical-image path");
    std::puts("usb_rom_volume_tests: PASS");
    return 0;
}
