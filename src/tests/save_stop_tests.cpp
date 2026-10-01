/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <atomic>
#include "pico.h"
#include "test_support.h"
#include "test_flash.h"
#include "hardware/gpio.h"
#include "../platform/rp2350/qspi_save.h"
#include "../storage/fx3_save_journal.h"

static TestSuperFx fx;
static std::atomic<uint8_t> ram[fx3_save::PAYLOAD_SIZE]{};
static bool irq = false;
static bool returned = false;
static unsigned saves = 0;
static unsigned rom_reads = 0;
bool snes_bus_usb_mode() { return false; }
void snes_pio_pause() { test_fail("live SAVE must not assert reset or pause PIO"); }
void snes_pio_resume() { test_fail("live SAVE must not assert reset or pause PIO"); }

static uint8_t rom_read(void*, uint32_t) {
    test_require(sdk_flash::xip && !sdk_flash::safe, "guest fetch during save");
    ++rom_reads;
    return 0x01;
}
static uint8_t ram_read(void*, uint32_t offset) { return ram[offset].load(); }
static void ram_write(void*, uint32_t offset, uint8_t value) { ram[offset].store(value); }
static void set_irq(void*, bool asserted) {
    if (asserted) {
        test_require(returned && !sdk_flash::safe && sdk_flash::xip &&
                         !sdk_flash::other_core_parked && !fx.running() && !fx.state().r[15],
                     "completion IRQ preceded durable save/XIP restoration/core release");
    }
    irq = asserted;
}
static bool save(void*) {
    ++saves;
    test_require(!fx.running() && fx.state().r[15] != 0 && !irq &&
                     !fx.state().ram_delay && !fx.state().rom_delay &&
                     !fx.state().primary_cache.valid_bits && !fx.state().secondary_cache.valid_bits,
                 "SAVE did not stop/drain before storage entry");
    const unsigned reads = rom_reads;
    const bool ok = qspi_save_now(nullptr);
    test_require(rom_reads == reads, "SAVE issued a guest fetch");
    test_require(!sdk_flash::safe && sdk_flash::xip && !sdk_flash::other_core_parked,
                 "SAVE returned before flash safety cleanup");
    returned = true;
    return ok;
}
static void check_locked() {
    test_require(sdk_flash::safe && sdk_flash::other_core_parked &&
                     sdk_flash::interrupts_disabled && !irq, "save lockout/IRQ ordering failed");
    test_require(!qspi_save_now(nullptr), "concurrent writer was not rejected");
    // This is the last allowed RAM mutation before snapshot capture.
    ram[55].store(0xCA);
}
static void check_mutation() {
    test_require(sdk_flash::safe && !sdk_flash::xip && sdk_flash::other_core_parked &&
                     sdk_flash::interrupts_disabled && !irq && !fx.running() && fx.state().r[15],
                 "flash mutation escaped lockout or signalled early completion");
}
static FxBackend backend() {
    return {nullptr, rom_read, nullptr, ram_read, ram_write, set_irq, save};
}
static void prepare(bool compatibility = false, bool irq_disabled = false) {
    FxConfig config = fx3_config;
    config.fx3_completion_irq = compatibility;
    fx.init(config, backend());
    fx.cpu_write(0x703A, 1); // 4bpp for pending plot-cache retirement.
    fx.cpu_write(0x7038, 8);
    fx.cpu_write(0x701E, 0x34);
    fx.cpu_write(0x701F, 0x12);
    fx.state_.program_read_buffer = 0x3F; // Execute ALT3 through the real pipeline.
    fx.execute();
    fx.state_.program_read_buffer = 0x00;
    fx.state_.irq_disabled = irq_disabled;
    fx.state_.ram_delay = 6;
    fx.state_.ram_write_address = 42;
    fx.state_.ram_write_value = 0xA5;
    fx.state_.secondary_cache = {0, 0, {1}, 1};
    fx.state_.primary_cache = {8, 0, {2}, 1};
    returned = false;
}
static void check_prefix() {
    test_require(!fx.state().flags.alt1 && !fx.state().flags.alt2 &&
                     !fx.state().flags.prefix && !fx.state().src_reg && !fx.state().dst_reg,
                 "SAVE_AND_STOP did not use common prefix cleanup");
}
int main() {
    sdk_test::reset_hardware();
    sdk_test::set_gpio_level(SNES_PRES_N_PIN, false);
    sdk_test::set_gpio_level(SNES_I_RESET_N_PIN, true);
    sdk_flash::bytes.fill(0xFF);
    qspi_save_init(ram);
    sdk_flash::check_locked = check_locked;
    sdk_flash::check_mutation = check_mutation;
    prepare();
    fx.run_unlimited(32);
    test_require(saves == 1 && returned && !fx.running() && !fx.state().r[15] &&
                     !fx.state().save_failed && !irq && sdk_flash::programs == 2,
                 "official SAVE_AND_STOP failed");
    check_prefix();
    // Boot restores the drained store, both plot caches, and locked snapshot data.
    ram[42].store(0); ram[55].store(0); ram[0x2000].store(0); ram[0x2201].store(0);
    qspi_save_init(ram);
    test_require(ram[42] == 0xA5 && ram[55] == 0xCA && ram[0x2000] == 1 && ram[0x2201] == 1,
                 "durable save omitted pending store/cache or locked snapshot");

    prepare(true);
    fx.execute();
    test_require(irq && fx.state().flags.irq && sdk_flash::programs == 2,
                 "compatibility completion/unchanged-save suppression failed");
    check_prefix();
    prepare(true, true);
    fx.execute();
    test_require(!irq, "disabled compatibility IRQ asserted");

    for (bool entry_failure : {true, false}) {
        prepare(true);
        ram[123].store(entry_failure ? 0x11 : 0x22);
        sdk_flash::fail_enter = entry_failure;
        sdk_flash::corrupt_program = !entry_failure;
        fx.execute();
        test_require(fx.state().save_failed && !fx.running() && fx.state().r[15] != 0 &&
                         !irq && !qspi_save_last_ok(), "failed save published success");
        check_prefix();
        sdk_flash::fail_enter = false;
        sdk_flash::corrupt_program = false;
    }
    prepare(true);
    fx.execute();
    test_require(!fx.state().save_failed && irq, "SAVE retry did not recover");

    // Missing backend fails closed, including R15 overflow into the success sentinel.
    auto missing = backend(); missing.save = nullptr;
    fx.init(fx3_config, missing);
    fx.state_.flags.running = true;
    fx.state_.flags.alt1 = fx.state_.flags.alt2 = true;
    fx.state_.program_read_buffer = 0;
    fx.state_.r[15] = 0;
    fx.execute();
    test_require(fx.state().save_failed && fx.state().r[15] == 0xFFFF && !fx.running(),
                 "missing save callback or wrapped PC reported success");
    check_prefix();

    // Plain STOP and ALT1/ALT2 STOP never save; legacy ALT3 STOP keeps its IRQ.
    const unsigned previous = saves;
    for (unsigned prefix = 0; prefix < 3; ++prefix) {
        fx.init(fx3_config, backend());
        fx.state_.flags.alt1 = (prefix & 1u) != 0;
        fx.state_.flags.alt2 = (prefix & 2u) != 0;
        fx.execute_opcode(0x00);
        test_require(!fx.running() && !fx.state().r[15] && !irq && saves == previous,
                     "non-SAVE STOP changed semantics");
        check_prefix();
    }
    TestMemory legacy;
    fx.init(fx1_config, make_test_backend(legacy));
    fx.state_.flags.alt1 = fx.state_.flags.alt2 = true;
    fx.execute_opcode(0x00);
    test_require(legacy.irq && saves == previous, "legacy ALT3 STOP became a save");
    check_prefix();
    std::puts("save_stop_tests: PASS");
}
