#define main volume_baseline_main
#include "usb_rom_volume_tests.cpp"
#undef main

static void root(UsbRomVolume& volume, uint32_t size, uint16_t cluster = 3) {
    std::array<uint8_t, 512> b{};
    std::memcpy(b.data(), "GAME    SFC", 11);
    b[11] = 0x20;
    put16(b.data(), 26, cluster);
    put32(b.data(), 28, size);
    test_require(volume.write(65, b.data(), b.size()), "root write failed");
}

static bool strict_write(void* p, uint32_t o, const uint8_t* b, size_t n) {
    return !static_cast<Sink*>(p)->completed && write(p, o, b, n);
}
static bool ordered_complete(void* p, UsbRomFileType t, uint32_t n,
                             const uint16_t* pages, uint32_t count) {
    test_require(count == 2 && pages[0] == 3 && pages[1] == 0,
                 "fragmented backwards FAT chain lost its order");
    return complete(p, t, n, pages, count);
}

int main() {
    Sink sink;
    sink.bytes.resize(UsbRomVolume::MAX_STAGING_PAGES * 4096u, 0xFF);
    UsbRomVolume volume({&sink, begin, strict_write, read, complete, flush});
    std::array<uint8_t, 512> block{};
    block.fill(0xA5);
    test_require(volume.write(89, block.data(), block.size()), "data before root lost");
    root(volume, 512);
    test_require(volume.flush() && !sink.completed, "flush finalized growing file");
    block.fill(0x5A);
    test_require(volume.write(90, block.data(), block.size()), "growing file rejected");
    root(volume, 1024);
    test_require(!volume.eject(), "missing FAT accepted");
    block.fill(0);
    put16(block.data(), 6, 0xFFFF);
    test_require(volume.write(1, block.data(), block.size()), "late FAT failed");
    test_require(!sink.completed && volume.eject() && sink.completed == 1024 &&
                     sink.bytes[0] == 0xA5 && sink.bytes[512] == 0x5A,
                 "retained upload not completed on eject");
    test_require(!volume.write(89, block.data(), block.size()), "post-eject write accepted");

    sink.completed = 0;
    UsbRomVolume fragmented({&sink, begin, strict_write, read, ordered_complete, flush});
    block.fill(0x31);
    test_require(fragmented.write(89, block.data(), block.size()), "late cluster data lost");
    block.fill(0x62);
    for (uint32_t i = 0; i < 8; ++i)
        test_require(fragmented.write(113u + i, block.data(), block.size()), "early cluster data lost");
    root(fragmented, 4608, 6);
    block.fill(0);
    put16(block.data(), 12, 3);
    put16(block.data(), 6, 0xFFFF);
    test_require(fragmented.write(1, block.data(), block.size()) && fragmented.eject(),
                 "fragmented eject failed");

    volume.reset();
    sink.completed = 0;
    root(volume, 8192);
    block.fill(0x55);
    for (uint32_t i = 0; i < 8; ++i)
        test_require(volume.write(89u + i, block.data(), block.size()), "cycle setup failed");
    block.fill(0);
    put16(block.data(), 6, 3);
    test_require(volume.write(1, block.data(), block.size()) && !volume.eject(), "cyclic FAT accepted");
    put16(block.data(), 6, 4);
    put16(block.data(), 8, 0xFFFF);
    test_require(volume.write(1, block.data(), block.size()) && !volume.eject(), "missing blocks accepted");
    test_require(!volume.write(volume.block_count(), block.data(), block.size()), "bounds ignored");
    test_require(volume.write(volume.block_count() - 1, block.data(), block.size()), "spill page rejected");
    std::puts("usb_block_order_tests: PASS");
}
