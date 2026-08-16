#include <cstdint>
#include <cstdio>

#include "../platform/rp2350/fx_sync.h"
#include "test_support.h"

// fx_sync publishes legacy GSU ROM ownership to the PIO layer. Host synchronization
// tests record the request without trying to emulate PIO timing.
static bool pio_rom_blocked = false;
static uint32_t pio_rom_updates = 0;
void snes_pio_request_rom_ownership(bool blocked) {
    pio_rom_blocked = blocked;
    ++pio_rom_updates;
}

static void test_start_and_snapshot(SuperFx& fx) {
    fx_sync_cpu_write(0x703A, 0x18);
    fx_sync_cpu_write(0x701E, 0x00);
    fx_sync_cpu_write(0x701F, 0x00);
    test_require(fx.running(), "synchronized R15 write did not start FX3");
    test_require(fx_sync_cpu_read(0x701E) == 0 && fx_sync_cpu_read(0x701F) == 0,
                 "FX3 R15 snapshot does not match the started program counter");
}

static void test_queued_register_write(SuperFx& fx) {
    test_require(fx_sync_cpu_write(0x7038, 0x55), "running-state register write was not queued");
    test_require(fx_sync_core1_service(), "core 1 did not service the running FX3");
    test_require(fx.state().screen_base == 0x40,
                 "queued register write bypassed the running-state write restriction");
}

static void test_running_ram_access() {
    fx_sync_cpu_ram_write(0x1234, 0xA5);
    test_require(fx_sync_cpu_ram_read(0x1234) == 0xA5,
                 "synchronized FX3 RAM access failed while core 1 owned the core state");
}

static void test_stop_handoff(SuperFx& fx) {
    test_require(fx_sync_core1_service(), "core 1 did not execute the pending STOP");
    test_require(!fx.running(), "FX3 STOP did not halt execution");
    test_require(fx.state().r[15] == 0, "FX3 STOP did not publish R15=0");
    test_require(!fx_sync_core1_service(), "core 1 retained ownership after stopped state had no work");
}

static void test_queued_reset(SuperFx& fx) {
    test_require(fx_sync_cpu_write(0x701E, 0x20), "second start low-byte write failed");
    test_require(fx_sync_cpu_write(0x701F, 0x00), "second start high-byte write failed");
    test_require(fx.running(), "second synchronized start failed");

    test_require(fx_sync_reset(), "queued reset was unexpectedly rejected");
    test_require(fx_sync_core1_service(), "queued reset was not serviced by core 1");
    test_require(!fx.running() && fx.state().r[15] == 0,
                 "queued reset did not return the FX3 core to reset state");
}

static void test_reset_retry_after_full_queue(SuperFx& fx) {
    test_require(fx_sync_cpu_write(0x703A, 0x18), "queue saturation SCMR write failed");
    test_require(fx_sync_cpu_write(0x701E, 0x20), "queue saturation start low-byte failed");
    test_require(fx_sync_cpu_write(0x701F, 0x00), "queue saturation start high-byte failed");
    test_require(fx.running(), "queue-saturation setup did not start FX3");

    for (uint16_t i = 0; i < 255; ++i) {
        test_require(fx_sync_cpu_write(0x7038, static_cast<uint8_t>(i)),
                     "command queue filled before its 255-command capacity");
    }

    test_require(!fx_sync_cpu_write(0x7038, 0xAA),
                 "register write unexpectedly entered a full command queue");
    test_require(!fx_sync_reset(), "reset unexpectedly entered a full command queue");
    test_require(fx_sync_core1_service(), "core 1 did not drain the saturated command queue");
    test_require(fx_sync_reset(), "reset was not accepted after queue space became available");
    test_require(fx_sync_core1_service(), "retried reset was not serviced by core 1");
    test_require(!fx.running(), "retried reset did not stop FX3");
}

static void test_direct_stopped_access(TestMemory& memory) {
    // With core 1 released, synchronized access should go directly through the core/backend.
    memory.ram[0x2222] = 0x61;
    test_require(fx_sync_cpu_ram_read(0x2222) == 0x61, "stopped synchronized RAM read failed");
    fx_sync_cpu_ram_write(0x2222, 0xC4);
    test_require(memory.ram[0x2222] == 0xC4, "stopped synchronized RAM write failed");
    test_require(fx_sync_rom_access_allowed() && fx_sync_ram_access_allowed(),
                 "stopped FX3 should leave CPU ROM/RAM access enabled");
    test_require(fx_sync_blocked_rom_value(0x0E) == 0x0C,
                 "synchronized blocked-ROM helper returned the wrong pattern");

    test_require(fx_sync_cpu_write(0x7000, 0x5A), "stopped register low-byte write failed");
    test_require(fx_sync_cpu_write(0x7001, 0x00), "stopped register high-byte write failed");
    test_require(fx_sync_cpu_read(0x7000) == 0x5A, "stopped synchronized register access failed");

    test_require(fx_sync_cpu_write(0x7400, 0xA6), "mirrored register low-byte write failed");
    test_require(fx_sync_cpu_write(0x7401, 0x00), "mirrored register high-byte write failed");
    test_require(fx_sync_cpu_read(0x7800) == 0xA6, "synchronized FX3 $400 mirror failed");
    test_require(fx_sync_cpu_read(0x7B00) == 0xFF, "synchronized FX3 open-bus quarter is wrong");
}

static void test_legacy_ownership_snapshots() {
    TestMemory memory{};
    memory.rom[0] = 0x00; // STOP after the synthetic reset NOP.

    SuperFx fx;
    const FxBackend backend = make_test_backend(memory);
    fx.init(fx2_config, backend);
    pio_rom_blocked = false;
    pio_rom_updates = 0;
    fx_sync_init(fx, backend);

    test_require(fx_sync_cpu_write(0x303A, 0x18), "legacy SCMR ownership write failed");
    test_require(fx_sync_cpu_write(0x301E, 0x00), "legacy start low-byte write failed");
    test_require(fx_sync_cpu_write(0x301F, 0x00), "legacy start high-byte write failed");
    test_require(!fx_sync_rom_access_allowed() && !fx_sync_ram_access_allowed(),
                 "legacy synchronized ownership did not block CPU ROM/RAM while running");
    test_require(pio_rom_blocked && pio_rom_updates != 0,
                 "legacy ROM ownership transition was not published to PIO");

    memory.ram[0x20] = 0x6A;
    test_require(fx_sync_cpu_ram_read(0x20) == 0xFF,
                 "blocked legacy synchronized RAM read did not return the open-bus value");
    fx_sync_cpu_ram_write(0x20, 0x99);
    test_require(memory.ram[0x20] == 0x6A,
                 "blocked legacy synchronized RAM write reached the backend");
    test_require(fx_sync_cpu_read(0x3000) == 0xFF,
                 "running legacy snapshot exposed an ordinary register");
    test_require(fx_sync_cpu_read(0x303B) == 0x04,
                 "legacy synchronized VCR snapshot is wrong");
    (void)fx_sync_cpu_read(0x3030);
    (void)fx_sync_cpu_read(0x3031); // IRQ=0 must not schedule a future clear.

    test_require(fx_sync_core1_service(), "legacy core did not execute synthetic NOP");
    test_require(fx_sync_core1_service(), "legacy core did not execute STOP");
    test_require(!fx.running() && memory.irq,
                 "legacy STOP did not halt and assert the modeled completion IRQ");
    test_require(fx_sync_rom_access_allowed() && fx_sync_ram_access_allowed(),
                 "legacy STOP did not return CPU ROM/RAM ownership");
    test_require(!pio_rom_blocked,
                 "legacy STOP did not publish ROM ownership release to PIO");

    // With core 1 released, SFR-high should clear the real core IRQ immediately.
    const uint8_t sfr_high = fx_sync_cpu_read(0x3031);
    test_require((sfr_high & 0x80) != 0 && !memory.irq,
                 "stopped synchronized SFR-high read did not acknowledge IRQ");
}

int main() {
    TestMemory memory{};
    memory.rom[0] = 0x00;
    memory.rom[0x20] = 0x00;

    SuperFx fx;
    const FxBackend backend = make_test_backend(memory);
    fx.init(fx3_config, backend);
    fx_sync_init(fx, backend);

    test_start_and_snapshot(fx);
    test_queued_register_write(fx);
    test_running_ram_access();
    test_stop_handoff(fx);
    test_queued_reset(fx);
    test_reset_retry_after_full_queue(fx);
    test_direct_stopped_access(memory);
    test_legacy_ownership_snapshots();

    std::puts("sync_tests: PASS");
    return 0;
}
