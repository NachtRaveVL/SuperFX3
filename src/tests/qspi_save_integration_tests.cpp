#include <atomic>
#include <cstdio>
#include "pico.h"
#include "hardware/gpio.h"
#include "test_flash.h"
#include "test_support.h"
#include "../platform/rp2350/qspi_save.h"
#include "../storage/fx3_save_journal.h"

static bool usb = false;
static bool busy = false;
static unsigned pauses = 0, resumes = 0;
bool snes_bus_usb_mode() { return usb; }
void snes_busy_irq_write(void*, bool asserted) { busy = asserted; }
void snes_pio_pause() { ++pauses; }
void snes_pio_resume() { ++resumes; }
static void check_busy() { test_require(busy, "QSPI mutation without busy IRQ"); }

int main() {
    static std::atomic<uint8_t> ram[fx3_save::PAYLOAD_SIZE]{};
    sdk_flash::bytes.fill(0xFF);
    sdk_flash::check_busy = check_busy;
    sdk_test::reset_hardware();
    sdk_test::set_gpio_level(SNES_PRES_N_PIN, false);
    sdk_test::set_gpio_level(SNES_I_RESET_N_PIN, true);
    qspi_save_init(ram);
    ram[42].store(0xA5);
    qspi_save_task();
    test_require(!sdk_flash::programs, "live game was paused to save");
    sdk_test::set_gpio_level(SNES_I_RESET_N_PIN, false);
    qspi_save_task();
    test_require(qspi_save_last_ok() && sdk_flash::programs == 2 && !busy && pauses == resumes,
                 "reset did not commit and release resources");
    qspi_save_task();
    test_require(sdk_flash::programs == 2, "held reset repeatedly wore flash");
    ram[42].store(0);
    qspi_save_init(ram);
    test_require(ram[42].load() == 0xA5, "boot restore failed");

    ram[42].store(0x99);
    sdk_flash::fail_enter = true;
    qspi_save_task();
    test_require(!qspi_save_last_ok() && !busy && pauses == resumes && sdk_flash::programs == 2,
                 "flash-safety failure was not handled");
    sdk_flash::fail_enter = false;
    sdk_test::set_gpio_level(SNES_I_RESET_N_PIN, true);
    qspi_save_task();
    sdk_test::set_gpio_level(SNES_I_RESET_N_PIN, false);
    qspi_save_task();
    test_require(qspi_save_last_ok() && sdk_flash::programs == 4, "save retry failed");

    ram[100].store(0x33);
    usb = true;
    sdk_test::set_gpio_level(SNES_PRES_N_PIN, true);
    qspi_save_task();
    test_require(qspi_save_last_ok() && sdk_flash::programs == 6 && !busy,
                 "USB-powered console disconnect did not save");
    ram[100].store(0);
    qspi_save_init(ram);
    test_require(ram[100].load() == 0x33, "disconnect save did not restore");
    std::puts("qspi_save_integration_tests: PASS");
}
