#include <cstdio>
#include <cstring>

#include "../platform/rp2350/fx3_audio_sd.h"
#include "test_support.h"

#if SUPERFX3_BOARD_REVISION >= 2
namespace {
bool mounted = false;
bool opened = false;
unsigned opens = 0;
unsigned closes = 0;
char last_path[32] {};
}

extern "C" bool fx3_audio_sd_mount() { return mounted; }
extern "C" bool fx3_audio_sd_open_file(const char* path) {
    ++opens;
    std::snprintf(last_path, sizeof(last_path), "%s", path);
    return opened;
}
extern "C" int32_t fx3_audio_sd_read_file(uint8_t*, uint32_t) { return -2; }
extern "C" void fx3_audio_sd_close_file() { ++closes; }
#endif

int main() {
    const Fx3AudioSource source = fx3_audio_sd_source();
#if SUPERFX3_BOARD_REVISION >= 2
    test_require(source.open && source.read && source.close,
                 "Rev-B source did not expose its driver hooks");
    test_require(!source.open(source.context, 0x1234) && opens == 0,
                 "unmounted SD attempted to open an audio file");
    mounted = true;
    test_require(!source.open(source.context, 0x1234) &&
                     std::strcmp(last_path, "/audio/1234.brr") == 0,
                 "missing file or hexadecimal asset path was mishandled");
    opened = true;
    test_require(source.open(source.context, 0xABCD) &&
                     std::strcmp(last_path, "/audio/ABCD.brr") == 0,
                 "Rev-B asset naming did not match the control protocol");
    uint8_t data = 0;
    test_require(source.read(source.context, &data, 1) == -2,
                 "SD adapter did not preserve a temporary read retry");
    source.close(source.context);
    test_require(closes == 1, "SD adapter did not close the file");
#else
    test_require(!source.open && !source.read && !source.close,
                 "Rev A exposed SD access without a chip-select pin");
#endif
    std::puts("audio_sd_tests: PASS");
    return 0;
}
