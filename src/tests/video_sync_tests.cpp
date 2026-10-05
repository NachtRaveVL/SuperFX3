#include "test_support.h"
#include "../platform/rp2350/fx_sync.h"

void snes_pio_request_rom_ownership(bool) {}
void qspi_bus_audio_cancel() {}
bool fx_backend_sd_safe(const FxBackend&) { return true; }

int main() {
    TestMemory memory;
    SuperFx fx;
    const FxBackend backend = make_test_backend(memory);
    fx.init(fx3_config, backend);
    fx_sync_init(fx, backend);
    test_require(fx_sync_video_acquire(), "stopped GSU did not grant video ownership");
    test_require(!fx_sync_video_acquire(), "two video sessions acquired the same RAM");
    memory.ram[0x10000] = 0x5A;
    fx_sync_cpu_ram_write(0x10000, 0xFF);
    fx_sync_cpu_ram_write(0x1234, 0x42);
    fx_sync_cpu_write(0x701F, 1);
    test_require(memory.ram[0x10000] == 0x5A && memory.ram[0x1234] == 0x42 && !fx.running(),
                 "normal CPU writes corrupted video staging or restarted the GSU");
    test_require(fx_sync_sd_begin(), "video did not permit an SRAM-only SD service window");
    fx_sync_sd_ram_write(0x10000, 0xCC);
    fx_sync_sd_cpu_write(0x701F, 1);
    fx_sync_sd_drain_writes();
    test_require(memory.ram[0x10000] == 0x5A && fx_sync_sd_ram_read(0x10000) == 0x5A &&
                     !fx.running(), "SD service bypassed the video ownership guard");
    fx_sync_core1_service();
    fx_sync_video_release();
    fx_sync_cpu_ram_write(0x10000, 0x33);
    fx_sync_cpu_write(0x703A, 0x18);
    fx_sync_cpu_write(0x701F, 1);
    test_require(memory.ram[0x10000] == 0x33 && fx.running() && !fx_sync_video_acquire(),
                 "video release failed to restore GSU execution / RAM writes");
    std::puts("video_sync_tests: PASS");
}
