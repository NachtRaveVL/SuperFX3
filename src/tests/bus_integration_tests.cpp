#include <cstdint>
#include <cstdio>
#include <deque>

#include "pico.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "test_hardware.h"

#include "../platform/rp2350/fx_sync.h"
#include "../platform/rp2350/snes_bus.h"
#include "../platform/rp2350/snes_pio.h"
#include "test_support.h"

static constexpr unsigned PIO0_INDEX = 0;
static constexpr unsigned PIO1_INDEX = 1;
static constexpr unsigned PIO2_INDEX = 2;
static constexpr unsigned SELECT_SM = 0;
static constexpr unsigned WRITE_ADDR_SM = 1;
static constexpr unsigned WRITE_SM = 0;
static constexpr unsigned RESET_SM = 1;
static constexpr unsigned READ_SM = 0;

static uint32_t capture_word(uint8_t bank, uint16_t addr, uint8_t data) {
    return static_cast<uint32_t>(bank) |
           (static_cast<uint32_t>(data) << SNES_CAPTURE_DATA_SHIFT) |
           (static_cast<uint32_t>(addr) << SNES_CAPTURE_ADDR_LO_SHIFT);
}

static void inject_write(uint8_t bank, uint16_t addr, uint8_t data) {
    sdk_test::set_pio_rx(PIO1_INDEX, WRITE_SM, capture_word(bank, addr, data));
    sdk_test::set_pio_interrupt(PIO1_INDEX, 1);
    sdk_test::trigger_irq(sdk_test::pio_irq_num(PIO1_INDEX));
}

static void put_address_on_bus(uint8_t bank, uint16_t addr) {
    const uint32_t address = (static_cast<uint32_t>(bank) << 16) | addr;
    const uint64_t pins = static_cast<uint64_t>(address & 0xFFFFu) |
                          (static_cast<uint64_t>(address >> 16) << SNES_ADDR_HI_BASE);
    sdk_test::set_gpio_mask(SNES_ADDR_MASK, pins);
}

static uint8_t inject_read(uint8_t bank, uint16_t addr) {
    put_address_on_bus(bank, addr);
    sdk_test::set_pio_interrupt(PIO2_INDEX, 0);
    sdk_test::trigger_irq(sdk_test::pio_irq_num(PIO2_INDEX));
    return static_cast<uint8_t>((sdk_test::pio[PIO2_INDEX].tx[READ_SM] >> 1) & 0xFFu);
}

static void init_bus(SuperFx& fx, TestMemory& memory, const FxConfig& config) {
    sdk_test::reset_hardware();
    const FxBackend backend = make_test_backend(memory);
    fx.init(config, backend);
    fx_sync_init(fx, backend);
    snes_bus_init();
    snes_bus_start(fx);
}

static void test_fx3_frontend_round_trip() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);

    test_require(sdk_test::gpio_level[SNES_DATA_DIR_PIN] == SNES_DATA_DIR_IN,
                 "bus startup did not leave DATA_DIR pointing into the RP2350");
    test_require(sdk_test::gpio_level[SNES_BUS_OE_N_PIN] == SNES_BUS_ENABLE,
                 "bus startup did not connect the cartridge transceivers");
    test_require(sdk_test::gpio_level[SNES_ROM0_OE_N_PIN] == SNES_ROM_DISABLE &&
                     sdk_test::gpio_level[SNES_ROM1_OE_N_PIN] == SNES_ROM_DISABLE,
                 "bus startup enabled a parallel ROM while idle");
    test_require(sdk_test::gpio_level[SNES_IRQ_N_PIN],
                 "bus startup asserted IRQ");
    test_require(sdk_test::pio[PIO0_INDEX].enabled[SELECT_SM] &&
                     sdk_test::pio[PIO0_INDEX].enabled[WRITE_ADDR_SM] &&
                     sdk_test::pio[PIO1_INDEX].enabled[WRITE_SM] &&
                     sdk_test::pio[PIO1_INDEX].enabled[RESET_SM] &&
                     sdk_test::pio[PIO2_INDEX].enabled[READ_SM],
                 "PIO startup did not enable the complete bus frontend");
    test_require(sdk_test::pio[PIO2_INDEX].x[READ_SM] == 56,
                 "FX3 read PIO did not receive the full $70/$71 bank compare value");

    inject_write(0x00, 0x7038, 0x55);
    test_require(fx.state().screen_base == 0x55,
                 "captured register write did not traverse PIO IRQ -> sync -> core rules");

    inject_write(0x70, 0x1234, 0xA5);
    test_require(memory.ram[0x1234] == 0xA5,
                 "captured shared-RAM write did not reach the backend");

    test_require(inject_read(0x00, 0x703B) == 0x52,
                 "PIO register read did not return the FX3 VCR response");
    test_require(inject_read(0x70, 0x1234) == 0xA5,
                 "PIO shared-RAM read did not return backend data");
}

static void test_noncartridge_reads_do_not_drive() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);
    for (uint8_t bank : {uint8_t{0x7E}, uint8_t{0x7F}}) {
        (void)inject_read(bank, 0x7000);
        test_require(sdk_test::pio[PIO2_INDEX].tx[READ_SM] == 0,
                     "cartridge drove a response for a SNES work-RAM read");
    }
    (void)inject_read(0x00, 0x7300);
    test_require((sdk_test::pio[PIO2_INDEX].tx[READ_SM] & 1) != 0,
                 "reserved cartridge register window lost its driven open-bus response");
}

static void test_write_address_dma_backpressure() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);
    // Execute the configured DMA endpoints/chains with bounded hardware FIFOs.
    // Hold the destination full, then resume it: every address must survive in order.
    std::deque<uint32_t> source;
    std::deque<uint32_t> destination;
    uint32_t produced = 0;
    uint32_t consumed = 0;
    for (unsigned tick = 0; tick < 512; ++tick) {
        if (produced < 32 && source.size() < 8)
            source.push_back(0x12340000u + produced++);
        for (unsigned channel = 0; channel < sdk_test::next_dma; ++channel) {
            auto& dma = sdk_test::dma[channel];
            if (!dma.running) continue;
            if (dma.dreq == pio_get_dreq(pio0, WRITE_ADDR_SM, false) && source.empty()) continue;
            if (dma.dreq == pio_get_dreq(pio1, WRITE_SM, true) && destination.size() == 4) continue;
            uint32_t value;
            if (dma.read_addr == &pio0->rxf[WRITE_ADDR_SM]) {
                test_require(!source.empty(), "DMA read an empty source FIFO");
                value = source.front();
                source.pop_front();
            } else {
                value = *static_cast<const volatile uint32_t*>(dma.read_addr);
            }
            if (dma.write_addr == &pio1->txf[WRITE_SM]) {
                test_require(destination.size() < 4, "write-address DMA overflowed the PIO1 TX FIFO");
                destination.push_back(value);
            } else {
                *static_cast<volatile uint32_t*>(dma.write_addr) = value;
            }
            if (dma.transfer_count != dma_encode_endless_transfer_count()) {
                test_require(dma.transfer_count == 1, "DMA model requires single-word chained transfers");
                dma.running = false;
                if (dma.chain_to != channel)
                    sdk_test::dma[dma.chain_to].running = true;
            }
        }
        if (tick >= 32 && tick % 3 == 0 && !destination.empty()) {
            test_require(destination.front() == 0x12340000u + consumed++, "DMA lost write-address ordering");
            destination.pop_front();
        }
    }
    test_require(consumed == 32, "DMA did not resume after destination backpressure");
}


static void test_fx3_never_steals_parallel_rom_bus() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);

    const uint8_t value = snes_rom_read(nullptr, 0x123456);
    test_require(value == 0xFF, "FX3 unexpectedly used the legacy parallel-ROM callback");
    test_require(sdk_test::pio[PIO0_INDEX].enabled[WRITE_ADDR_SM] &&
                     sdk_test::pio[PIO1_INDEX].enabled[WRITE_SM] &&
                     sdk_test::pio[PIO2_INDEX].enabled[READ_SM],
                 "FX3 ROM callback guard disturbed the live SNES bus frontend");
    test_require(sdk_test::gpio_level[SNES_BUS_OE_N_PIN] == SNES_BUS_ENABLE,
                 "FX3 ROM callback guard isolated the SNES bus");
}

static void test_legacy_physical_rom_transaction() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx2_config);

    constexpr uint32_t address = 0x923456;
    constexpr uint8_t expected = 0xA6;
#if SNES_PARALLEL_ROM_COUNT == 2
    constexpr unsigned selected_rom_pin = SNES_ROM1_OE_N_PIN;
    constexpr unsigned unselected_rom_pin = SNES_ROM0_OE_N_PIN;
#else
    constexpr unsigned selected_rom_pin = SNES_ROM0_OE_N_PIN;
    constexpr unsigned unselected_rom_pin = SNES_ROM1_OE_N_PIN;
#endif
    sdk_test::set_gpio_mask(SNES_DATA_MASK, static_cast<uint64_t>(expected) << SNES_DATA_BASE);
    const uint32_t selected_before = sdk_test::gpio_low_count[selected_rom_pin];
    const uint32_t unselected_before = sdk_test::gpio_low_count[unselected_rom_pin];

    // The real core-0 loop grants and releases ownership while core 1 waits in
    // snes_rom_read(). The hook lets one thread deterministically model those
    // service opportunities without hiding the handshake behind a fake callback.
    sdk_test::tight_loop_hook = snes_bus_service;
    const uint8_t actual = snes_rom_read(nullptr, address);
    sdk_test::tight_loop_hook = nullptr;

    test_require(actual == expected, "legacy private ROM read sampled the wrong data byte");
    test_require(sdk_test::gpio_low_count[selected_rom_pin] == selected_before + 1,
                 "legacy private ROM read enabled the wrong physical ROM");
    test_require(sdk_test::gpio_low_count[unselected_rom_pin] == unselected_before,
                 "legacy private ROM read enabled both physical ROMs");
    test_require(sdk_test::busy_wait_cycles != 0,
                 "legacy private ROM read skipped its setup/access/hold waits");

    const uint64_t expected_address = static_cast<uint64_t>(address & 0xFFFFu) |
                                      (static_cast<uint64_t>(address >> 16) << SNES_ADDR_HI_BASE);
    test_require((sdk_test::gpio_snapshot() & SNES_ADDR_MASK) == expected_address,
                 "legacy private ROM read drove the wrong split address-bus value");
    for (unsigned pin = 0; pin < sdk_test::GPIO_COUNT; ++pin) {
        const uint64_t bit = uint64_t{1} << pin;
        if (SNES_ADDR_MASK & bit)
            test_require(!sdk_test::gpio_dir[pin], "legacy private ROM read did not release an address pin");
    }

    test_require(sdk_test::gpio_level[SNES_DATA_DIR_PIN] == SNES_DATA_DIR_IN &&
                     sdk_test::gpio_level[SNES_BUS_OE_N_PIN] == SNES_BUS_ENABLE &&
                     sdk_test::gpio_level[SNES_ROM0_OE_N_PIN] == SNES_ROM_DISABLE &&
                     sdk_test::gpio_level[SNES_ROM1_OE_N_PIN] == SNES_ROM_DISABLE,
                 "legacy private ROM read did not restore the safe listening state");
    test_require(sdk_test::pio[PIO0_INDEX].enabled[WRITE_ADDR_SM] &&
                     sdk_test::pio[PIO1_INDEX].enabled[WRITE_SM] &&
                     sdk_test::pio[PIO2_INDEX].enabled[READ_SM],
                 "legacy private ROM read did not restore PIO service");
}

static void test_pause_resume_safety() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);

    snes_pio_pause();
    test_require(!sdk_test::pio[PIO0_INDEX].enabled[WRITE_ADDR_SM] &&
                     !sdk_test::pio[PIO1_INDEX].enabled[WRITE_SM] &&
                     !sdk_test::pio[PIO2_INDEX].enabled[READ_SM],
                 "PIO pause left a bus-driving transaction state machine enabled");
    test_require(sdk_test::pio[PIO0_INDEX].enabled[SELECT_SM] &&
                     sdk_test::pio[PIO1_INDEX].enabled[RESET_SM],
                 "PIO pause disabled the selector or reset watcher");
    test_require(sdk_test::gpio_level[SNES_DATA_DIR_PIN] == SNES_DATA_DIR_IN &&
                     sdk_test::gpio_level[SNES_BUS_OE_N_PIN] == SNES_BUS_DISABLE &&
                     sdk_test::gpio_level[SNES_ROM0_OE_N_PIN] == SNES_ROM_DISABLE &&
                     sdk_test::gpio_level[SNES_ROM1_OE_N_PIN] == SNES_ROM_DISABLE,
                 "PIO pause did not isolate the cartridge bus safely");

    snes_pio_resume();
    test_require(sdk_test::pio[PIO0_INDEX].enabled[WRITE_ADDR_SM] &&
                     sdk_test::pio[PIO1_INDEX].enabled[WRITE_SM] &&
                     sdk_test::pio[PIO2_INDEX].enabled[READ_SM],
                 "PIO resume did not restart transaction state machines");
    test_require(sdk_test::gpio_level[SNES_DATA_DIR_PIN] == SNES_DATA_DIR_IN &&
                     sdk_test::gpio_level[SNES_BUS_OE_N_PIN] == SNES_BUS_ENABLE &&
                     sdk_test::gpio_level[SNES_ROM0_OE_N_PIN] == SNES_ROM_DISABLE &&
                     sdk_test::gpio_level[SNES_ROM1_OE_N_PIN] == SNES_ROM_DISABLE,
                 "PIO resume did not restore the safe listening state");
    test_require(sdk_test::pio[PIO2_INDEX].x[READ_SM] == 56,
                 "PIO resume lost the FX3 shared-RAM decode constant");
}

static void test_pio_reset_reaches_core1() {
    TestMemory memory{};
    memory.rom[0] = 0x01;
    SuperFx fx;
    init_bus(fx, memory, fx3_config);

    inject_write(0x00, 0x703A, 0x18);
    inject_write(0x00, 0x701E, 0x00);
    inject_write(0x00, 0x701F, 0x00);
    test_require(fx.running(), "PIO start sequence did not start FX3");

    sdk_test::set_pio_interrupt(PIO1_INDEX, 2);
    sdk_test::trigger_irq(sdk_test::pio_irq_num(PIO1_INDEX));
    test_require(snes_pio_reset_pending(), "PIO reset IRQ did not latch a reset request");

    snes_bus_service();
    test_require(!snes_pio_reset_pending(), "bus service did not accept the latched reset");
    test_require(fx.running(), "queued reset mutated the core before core 1 serviced it");
    test_require(fx_sync_core1_service(), "core 1 did not service the queued PIO reset");
    test_require(!fx.running() && fx.state().r[15] == 0,
                 "PIO reset did not propagate through bus service and synchronization to the core");
    test_require(sdk_test::gpio_level[SNES_IRQ_N_PIN],
                 "reset completion left the active-low cartridge IRQ asserted");
}

static void test_legacy_ownership_reaches_read_pio() {
    TestMemory memory{};
    memory.rom[0] = 0x00;
    SuperFx fx;
    init_bus(fx, memory, fx2_config);

    test_require(sdk_test::pio[PIO2_INDEX].x[READ_SM] == 56,
                 "legacy read PIO started with ROM incorrectly blocked");

    inject_write(0x00, 0x303A, 0x18);
    inject_write(0x00, 0x301E, 0x00);
    inject_write(0x00, 0x301F, 0x00);
    test_require(fx.running(), "legacy PIO start sequence did not start the GSU");
    test_require(sdk_test::pio[PIO2_INDEX].x[READ_SM] == 0,
                 "GSU ROM ownership did not propagate into the live read PIO state");

    test_require(fx_sync_core1_service(), "legacy core did not execute synthetic NOP");
    test_require(fx_sync_core1_service(), "legacy core did not execute STOP");
    test_require(!fx.running(), "legacy core did not stop");
    test_require(sdk_test::pio[PIO2_INDEX].x[READ_SM] == 56,
                 "GSU STOP did not return direct-ROM ownership to the read PIO state");
}

static void test_post_reset_write_cannot_overtake_reset() {
    for (bool running : {false, true}) {
        TestMemory memory{};
        SuperFx fx;
        init_bus(fx, memory, fx3_config);
        if (running) {
            inject_write(0x00, 0x701E, 0x00);
            inject_write(0x00, 0x701F, 0x00);
        }
        // Reset was latched, but the main loop has not yet had CPU time.
        sdk_test::set_pio_interrupt(PIO1_INDEX, 2);
        sdk_test::trigger_irq(sdk_test::pio_irq_num(PIO1_INDEX));
        inject_write(0x00, 0x7038, 0x55);
        snes_bus_service();
        fx_sync_core1_service();
        test_require(fx.state().screen_base == 0x55,
                     "post-reset register write overtook reset and was lost");
    }
}

static void test_reset_order_survives_full_command_queue() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);
    inject_write(0x00, 0x701E, 0x00);
    inject_write(0x00, 0x701F, 0x00);
    unsigned queued = 0;
    while (queued < 256 && fx_sync_cpu_write(0x703A, 0x07)) ++queued;
    test_require(queued == 255, "test did not fill the command queue");
    sdk_test::set_pio_interrupt(PIO1_INDEX, 2);
    sdk_test::trigger_irq(sdk_test::pio_irq_num(PIO1_INDEX));
    snes_bus_service();
    test_require(snes_pio_reset_pending(), "full queue lost the pending reset");

    sdk_test::tight_loop_hook = [] { (void)fx_sync_core1_service(); };
    inject_write(0x00, 0x7038, 0x55);
    sdk_test::tight_loop_hook = nullptr;
    fx_sync_core1_service();
    test_require(!snes_pio_reset_pending() && fx.state().screen_base == 0x55,
                 "queue saturation lost reset or reordered the post-reset write");
}

int main() {
    test_noncartridge_reads_do_not_drive();
    test_write_address_dma_backpressure();
    test_fx3_frontend_round_trip();
    test_fx3_never_steals_parallel_rom_bus();
    test_legacy_physical_rom_transaction();
    test_pause_resume_safety();
    test_pio_reset_reaches_core1();
    test_post_reset_write_cannot_overtake_reset();
    test_reset_order_survives_full_command_queue();
    test_legacy_ownership_reaches_read_pio();

    std::puts("bus_integration_tests: PASS");
    return 0;
}
