#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "../video/fx3_video_stream.h"
#include "test_support.h"

namespace {
std::array<std::atomic<uint8_t>, 128u * 1024u> ram;
std::vector<uint8_t> file;
size_t position = 0;
bool available = true, acquired = false, retry = false;
unsigned reads = 0;
bool acquire() { if (!available || acquired) return false; acquired = true; return true; }
void release() { acquired = false; }
bool open(void*, uint16_t asset) { position = 0; return asset == 0x1234; }
int32_t read(void*, uint8_t* data, uint32_t size) {
    ++reads;
    test_require(size <= 512, "video exceeded its per-service SD burst");
    if (retry) return -2;
    const size_t count = std::min(static_cast<size_t>(size), file.size() - position);
    std::memcpy(data, file.data() + position, count);
    position += count;
    return static_cast<int32_t>(count);
}
void close(void*) {}
void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value)); out.push_back(static_cast<uint8_t>(value >> 8));
}
void put32(std::vector<uint8_t>& out, uint32_t value) {
    put16(out, static_cast<uint16_t>(value)); put16(out, static_cast<uint16_t>(value >> 16));
}
uint32_t crc(const std::vector<uint8_t>& data) {
    uint32_t result = UINT32_MAX;
    for (uint8_t value : data) {
        result ^= value;
        for (unsigned bit = 0; bit < 8; ++bit)
            result = (result >> 1) ^ ((result & 1u) ? 0xEDB88320u : 0u);
    }
    return result ^ UINT32_MAX;
}
void make_file(bool compressed, uint32_t count = 3) {
    file.clear(); put32(file, 0x56584653); file.push_back(1); file.push_back(1);
    put16(file, 256); put16(file, 224); put16(file, 32);
    put32(file, count); put32(file, count * 50u); put32(file, 16000); put32(file, 0); put32(file, 0);
    for (uint32_t i = 0; i < count; ++i) {
        std::vector<uint8_t> payload(32u + fx3_video::MAP_SIZE + 32u, 0);
        payload[0] = static_cast<uint8_t>(i + 1u);
        std::vector<uint8_t> encoded;
        if (compressed) {
            encoded.push_back(0); encoded.push_back(payload[0]);
            size_t rest = payload.size() - 1u;
            while (rest) {
                const size_t run = std::min(rest, size_t{128});
                if (run == 1) { encoded.push_back(0); encoded.push_back(0); }
                else { encoded.push_back(static_cast<uint8_t>(257u - run)); encoded.push_back(0); }
                rest -= run;
            }
        } else encoded = payload;
        put32(file, i * 50u); put16(file, 1); file.push_back(compressed ? 1 : 0); file.push_back(0);
        put32(file, static_cast<uint32_t>(encoded.size()));
        put32(file, static_cast<uint32_t>(payload.size())); put32(file, crc(payload)); put32(file, 50);
        file.insert(file.end(), encoded.begin(), encoded.end());
    }
}
void write(uint8_t reg, uint8_t value) { fx3_video_host_write(fx3_video::MMIO_BASE + reg, value); }
uint8_t get(uint8_t reg) { return fx3_video_host_read(fx3_video::MMIO_BASE + reg); }
void task(unsigned count = 32) { for (unsigned i = 0; i < count; ++i) fx3_video_task(); }
void start() {
    fx3_video_init({nullptr, open, read, close}, ram.data(), acquire, release);
    write(fx3_video::AssetIdLow, 0x34); write(fx3_video::AssetIdHigh, 0x12);
    write(fx3_video::Command, fx3_video::Play); task();
}
}

int main(int argc, char** argv) {
#if SUPERFX3_AUDIO_SD
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary);
        file.assign(std::istreambuf_iterator<char>(input), {});
        test_require(file.size() >= 32, "converter integration fixture was missing");
        start();
        unsigned count = 0;
        for (unsigned work = 0; work < 10000; ++work) {
            const uint8_t page = get(fx3_video::Page);
            if (page < 2) {
                write(fx3_video::Page, page); ++count;
                write(fx3_video::Page, fx3_video::NO_PAGE);
            }
            fx3_video_task();
            if (get(fx3_video::Status) == static_cast<uint8_t>(Fx3VideoStatus::Error))
                test_fail("converter produced a container rejected by the firmware");
            if (get(fx3_video::Status) == static_cast<uint8_t>(Fx3VideoStatus::Eof) &&
                get(fx3_video::Page) == fx3_video::NO_PAGE) break;
        }
        const uint32_t expected = file[12] | (static_cast<uint32_t>(file[13]) << 8) |
            (static_cast<uint32_t>(file[14]) << 16) | (static_cast<uint32_t>(file[15]) << 24);
        test_require(count == expected, "firmware did not publish every converted frame");
        std::puts("video converter/firmware integration: PASS");
        return 0;
    }
    for (bool compressed : {false, true}) {
        for (auto& value : ram) value.store(0xA5);
        make_file(compressed); start();
        test_require(acquired && fx3_video_gsu_locked() && get(fx3_video::Page) == 0,
                     "video did not acquire stopped GSU / publish the first page");
        test_require(ram[0].load() == 0xA5 && ram[0x10000].load() == 1 &&
                         ram[0x18000].load() == 2, "video decoding damaged bank $70 or page layout");
        const size_t held_position = position;
        task(); test_require(position == held_position, "video overwrote a READY page");
        write(fx3_video::Page, 1);
        test_require(get(fx3_video::ActivePage) == 0xFF, "out-of-order page claim succeeded");
        write(fx3_video::Page, 0); task();
        test_require(get(fx3_video::ActivePage) == 0 && ram[0x10000].load() == 1,
                     "ACTIVE page changed during staging");
        write(fx3_video::Page, 1); task();
        test_require(ram[0x10000].load() == 3 && get(fx3_video::Status) ==
                         static_cast<uint8_t>(Fx3VideoStatus::Eof), "last frame or EOF was lost");
        test_require(get(fx3_video::Pts) == 50 && get(fx3_video::FrameDuration) == 50,
                     "frame timing did not follow the claimed immutable page");
        write(fx3_video::Command, fx3_video::Stop); task();
        test_require(acquired && get(fx3_video::ActivePage) == 1 && ram[0x18000].load() == 2,
                     "STOP released/modified an active DMA page");
        write(fx3_video::Command, fx3_video::Release); task();
        test_require(!acquired && !fx3_video_gsu_locked(), "explicit release retained the GSU lock");
    }
    make_file(false, 1); retry = true; start();
    test_require(position == 0 && acquired, "temporary read retry advanced the file");
    retry = false; task(); test_require(get(fx3_video::Page) == 0, "retry did not recover");
    fx3_video_request_reset(); task(); test_require(!acquired, "reset retained frame ownership");

    make_file(false, 1); file[56] ^= 1; start();
    test_require(get(fx3_video::Error) == static_cast<uint8_t>(Fx3VideoError::BadCrc) &&
                     get(fx3_video::Page) == 0xFF, "bad CRC reached SNES DMA");
    fx3_video_request_reset(); task();
    make_file(true, 1); file[56] = 128; start();
    test_require(get(fx3_video::Error) == static_cast<uint8_t>(Fx3VideoError::InvalidFormat),
                 "invalid PackBits tag was accepted");
    fx3_video_request_reset(); task();
    make_file(false, 1); file.resize(70); start();
    test_require(get(fx3_video::Error) == static_cast<uint8_t>(Fx3VideoError::ReadFailed),
                 "truncated file was treated as successful EOF");
    fx3_video_request_reset(); task();
    available = false; make_file(false, 1); start();
    test_require(!acquired && get(fx3_video::Error) == static_cast<uint8_t>(Fx3VideoError::Busy),
                 "video acquired a running GSU");
    available = true;
    fx3_video_init({}, ram.data(), acquire, release);
    write(fx3_video::Command, fx3_video::Play); task();
    test_require(!acquired && get(fx3_video::Error) == static_cast<uint8_t>(Fx3VideoError::Unavailable),
                 "missing source did not fail safely");
#else
    (void)argc; (void)argv;
    fx3_video_init({}, nullptr, nullptr, nullptr);
    write(fx3_video::Command, fx3_video::Play);
    test_require(get(fx3_video::Error) == static_cast<uint8_t>(Fx3VideoError::Unavailable),
                 "Rev A attempted SD video");
#endif
    std::puts("video_stream_tests: PASS");
}
