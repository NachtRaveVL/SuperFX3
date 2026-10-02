#include <cstdint>
#include <cstdio>
#include <deque>

#include "pico.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "test_hardware.h"

#include "../platform/rp2350/fx_sync.h"
#include "../platform/rp2350/snes_bus.h"
#include "../platform/rp2350/snes_bus_layout.h"
#include "../platform/rp2350/snes_pio.h"
#include "test_support.h"

static constexpr unsigned PIO0_INDEX = 0;
static constexpr unsigned PIO1_INDEX = 1;
static constexpr unsigned PIO2_INDEX = 2;
static constexpr unsigned CONTROL_SM = 0;
static constexpr unsigned WRITE_ADDRESS_SM = 1;
static constexpr unsigned WRITE_TRIGGER_SM = 0;
static constexpr unsigned WRITE_CAPTURE_SM = 1;
static constexpr unsigned RESET_SM = 2;
static constexpr unsigned READ_SM = 0;

static uint32_t capture_word(uint8_t bank, uint16_t addr, uint8_t data) {
    const uint32_t address = (static_cast<uint32_t>(bank) << 16) | addr;
    return (snes_pack_address_raw(address) << SNES_CAPTURE_ADDR_RAW_SHIFT) |
           (static_cast<uint32_t>(snes_pack_data_raw(data)) << SNES_CAPTURE_DATA_SHIFT);
}

static void inject_write(uint8_t bank, uint16_t addr, uint8_t data) {
    sdk_test::set_pio_rx(PIO1_INDEX, WRITE_CAPTURE_SM, capture_word(bank, addr, data));
    sdk_test::set_pio_interrupt(PIO1_INDEX, 1);
    sdk_test::trigger_irq(sdk_test::pio_irq_num(PIO1_INDEX));
}

static void put_address_on_bus(uint8_t bank, uint16_t addr) {
    const uint32_t address = (static_cast<uint32_t>(bank) << 16) | addr;
    sdk_test::set_gpio_mask(SNES_ADDR_MASK, snes_address_to_gpio(address));
}

static uint32_t inject_read_word(uint8_t bank, uint16_t addr, bool cart_selected = false) {
    put_address_on_bus(bank, addr);
    sdk_test::set_gpio_level(SNES_I_CART_N_PIN, !cart_selected);
    sdk_test::set_pio_interrupt(PIO2_INDEX, 0);
    sdk_test::trigger_irq(sdk_test::pio_irq_num(PIO2_INDEX));
    return sdk_test::pio[PIO2_INDEX].tx[READ_SM];
}

static uint8_t inject_read(uint8_t bank, uint16_t addr) {
    const uint32_t response = inject_read_word(bank, addr);
    return snes_unpack_data_raw(static_cast<uint8_t>((response >> 1) & 0xFFu));
}

static void init_bus(SuperFx& fx, TestMemory& memory, const FxConfig& config) {
    sdk_test::reset_hardware();
    const FxBackend backend = make_test_backend(memory);
    fx.init(config, backend);
    fx_sync_init(fx, backend);
    snes_bus_init();
    snes_pio_set_rom_map(SnesRomMap::Fx3Physical);
    sdk_test::set_gpio_level(SNES_PRES_N_PIN, false);
    snes_bus_start(fx);
}

static bool control_bit(unsigned pin) {
    return sdk_test::gpio_level[pin];
}

static void require_control_word(uint8_t word, const char* message) {
    for (unsigned bit = 0; bit < SNES_LOCAL_CONTROL_COUNT; ++bit) {
        const bool expected = (word & (1u << bit)) != 0;
        if (control_bit(SNES_LOCAL_CONTROL_BASE + bit) != expected)
            test_require(false, message);
    }
}

static void require_translated_strobes_input(const char* message) {
    test_require(!sdk_test::gpio_dir[SNES_I_RD_N_PIN] &&
                     !sdk_test::gpio_dir[SNES_I_WR_N_PIN], message);
}

static void test_routed_packers() {
    const auto require_address_round_trip = [](uint32_t value) {
        test_require(snes_unpack_address_raw(snes_pack_address_raw(value)) == value,
                     "routed address pack/unpack lost a bit");
        test_require(snes_address_from_gpio(snes_address_to_gpio(value)) == value,
                     "routed address GPIO conversion lost a bit");
    };
    require_address_round_trip(0);
    require_address_round_trip(0x00FFFFFFu);
    for (unsigned bit = 0; bit < 24; ++bit) {
        require_address_round_trip(1u << bit);
        require_address_round_trip(0x00FFFFFFu ^ (1u << bit));
    }
    uint32_t address = 0x2350B5u;
    for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        address = (address * 1664525u + 1013904223u) & 0x00FFFFFFu;
        require_address_round_trip(address);
    }
    for (uint32_t value = 0; value < 256; ++value) {
        const auto byte = static_cast<uint8_t>(value);
        test_require(snes_unpack_data_raw(snes_pack_data_raw(byte)) == byte,
                     "routed data pack/unpack lost a bit");
        test_require(snes_data_from_gpio(snes_data_to_gpio(byte)) == byte,
                     "routed data GPIO conversion lost a bit");
    }
}

static void test_fx3_frontend_round_trip() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);

    require_control_word(SNES_CONTROL_CONSOLE_IDLE,
                         "startup did not select the console-listening control state");
    require_translated_strobes_input("startup drove /I_RD or /I_WR");
    test_require(!sdk_test::gpio_dir[SNES_I_RESET_N_PIN],
                 "startup left local /I_RST driving while /C_OE was enabled");
    test_require(sdk_test::gpio_level[SNES_O_IRQ_N_PIN],
                 "startup asserted /O_IRQ");
    test_require(sdk_test::pio[PIO0_INDEX].enabled[CONTROL_SM] &&
                     sdk_test::pio[PIO0_INDEX].enabled[WRITE_ADDRESS_SM] &&
                     sdk_test::pio[PIO1_INDEX].enabled[WRITE_TRIGGER_SM] &&
                     sdk_test::pio[PIO1_INDEX].enabled[WRITE_CAPTURE_SM] &&
                     sdk_test::pio[PIO1_INDEX].enabled[RESET_SM] &&
                     sdk_test::pio[PIO2_INDEX].enabled[READ_SM],
                 "PIO startup did not enable the routed bus frontend");
    test_require(sdk_test::pio[PIO2_INDEX].x[READ_SM] == SNES_CONTROL_CONSOLE_IDLE,
                 "read PIO did not retain the idle control word");

    inject_write(0x00, 0x7038, 0x55);
    test_require(fx.state().screen_base == 0x55,
                 "captured routed register write did not reach the core");

    inject_write(0x70, 0x1234, 0xA5);
    test_require(memory.ram[0x1234] == 0xA5,
                 "captured routed SRAM write did not reach the backend");

    test_require(inject_read(0x00, 0x703B) == 0x52,
                 "routed register read returned the wrong byte");
    test_require(inject_read(0x70, 0x1234) == 0xA5,
                 "routed SRAM read returned the wrong byte");

    snes_pio_set_rom_map(SnesRomMap::Fx3, 8u * 1024u);
    inject_write(0x70, 0x3234, 0x6C);
    test_require(memory.ram[0x1234] == 0x6C && inject_read(0x71, 0xF234) == 0x6C,
                 "declared SRAM size did not mirror the CPU-visible window");
    snes_pio_set_rom_map(SnesRomMap::Fx3, 0);
    inject_write(0x70, 0x1234, 0x99);
    test_require(memory.ram[0x1234] == 0x6C &&
                     inject_read_word(0x70, 0x1234, true) == 0,
                 "ROM-only cartridge exposed shared SRAM to the CPU");
}

static void test_noncartridge_reads_do_not_drive() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);
    for (uint8_t bank : {uint8_t{0x7E}, uint8_t{0x7F}}) {
        test_require(inject_read_word(bank, 0x7000) == 0,
                     "cartridge drove a response for a SNES work-RAM read");
    }
    test_require((inject_read_word(0x00, 0x7300) & 1u) != 0,
                 "reserved FX3 register window lost its driven open-bus response");
    test_require(inject_read_word(0x72, 0x8000, true) == 0,
                 "extended ROM bank was incorrectly disabled");
    snes_pio_set_rom_map(SnesRomMap::ExLoRom);
    for (uint8_t bank : {uint8_t{0x70}, uint8_t{0x71}, uint8_t{0x72}, uint8_t{0x7D}})
        test_require(inject_read_word(bank, 0x8000, true) == 0,
                     "ExLoROM upper-bank data hidden by SRAM/open bus");
    test_require(inject_read_word(0x70, 0x1000, true) != 0,
                 "ExLoROM low SRAM window missing");
    snes_pio_set_rom_map(SnesRomMap::ExHiRom);
    for (uint8_t bank : {uint8_t{0x70}, uint8_t{0x71}, uint8_t{0x72}, uint8_t{0x7D}})
        test_require(inject_read_word(bank, 0x1000, true) == 0,
                     "ExHiROM full-bank data hidden by SRAM/open bus");
    inject_write(0x20, 0x703B, 0xA6);
    test_require(inject_read(0x20, 0x703B) == 0xA6,
                 "ExHiROM SRAM shadowed by FX3 register mirrors");
}

static void test_dma_bridges() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);
    test_require(sdk_test::next_dma == 3, "routed bus did not claim three DMA bridges");

    const volatile void* address_source = &pio0->rxf[WRITE_ADDRESS_SM];
    volatile void* capture_sink = &pio1->txf[WRITE_CAPTURE_SM];
    bool found_address_bridge = false;
    for (unsigned channel = 0; channel < sdk_test::next_dma; ++channel) {
        const auto& dma = sdk_test::dma[channel];
        if (dma.read_addr == address_source && dma.write_addr == capture_sink && dma.running)
            found_address_bridge = true;
    }
    test_require(found_address_bridge,
                 "raw GPIO8-GPIO31 address DMA bridge is missing or reversed");
}

static void test_fx3_never_steals_parallel_rom_bus() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);

    const uint32_t ce_before = sdk_test::gpio_low_count[SNES_ROM_CE_N_PIN];
    test_require(snes_rom_read(nullptr, 0x123456) == 0xFF,
                 "FX3 unexpectedly used the parallel-ROM callback");
    test_require(sdk_test::gpio_low_count[SNES_ROM_CE_N_PIN] == ce_before,
                 "FX3 callback asserted the single ROM_CE# output");
    require_control_word(SNES_CONTROL_CONSOLE_IDLE,
                         "FX3 callback disturbed the listening control state");
}

static void test_legacy_physical_rom_transaction() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx2_config);

    constexpr uint32_t address = 0x923456;
    constexpr uint8_t expected = 0xA6;
    sdk_test::set_gpio_mask(SNES_DATA_MASK, snes_data_to_gpio(expected));
    const uint32_t ce_before = sdk_test::gpio_low_count[SNES_ROM_CE_N_PIN];
    const uint32_t rd_before = sdk_test::gpio_low_count[SNES_ROM_RD_N_PIN];
    const uint32_t wr_before = sdk_test::gpio_low_count[SNES_ROM_WR_N_PIN];

    sdk_test::tight_loop_hook = snes_bus_service;
    const uint8_t actual = snes_rom_read(nullptr, address);
    sdk_test::tight_loop_hook = nullptr;

    test_require(actual == expected, "legacy private ROM read sampled the wrong routed data byte");
    test_require(sdk_test::gpio_low_count[SNES_ROM_CE_N_PIN] == ce_before + 1,
                 "legacy private ROM read did not assert the single ROM_CE# pin");
    test_require(sdk_test::gpio_low_count[SNES_ROM_RD_N_PIN] == rd_before + 1,
                 "legacy private ROM read did not assert ROM RD# on GPIO1");
    test_require(sdk_test::gpio_low_count[SNES_ROM_WR_N_PIN] == wr_before,
                 "legacy private ROM read incorrectly asserted ROM WR#");
    test_require(sdk_test::busy_wait_cycles != 0,
                 "legacy private ROM read skipped setup/access/hold timing");
    test_require((sdk_test::gpio_snapshot() & SNES_ADDR_MASK) == snes_address_to_gpio(address),
                 "legacy private ROM read drove the wrong routed address");
    for (unsigned pin = SNES_ADDR_RAW_BASE;
         pin < SNES_ADDR_RAW_BASE + SNES_ADDR_RAW_COUNT; ++pin) {
        test_require(!sdk_test::gpio_dir[pin],
                     "legacy private ROM read did not release an address pin");
    }
    require_control_word(SNES_CONTROL_CONSOLE_IDLE,
                         "legacy private ROM read did not restore listening controls");
    require_translated_strobes_input("legacy private ROM read drove /I_RD or /I_WR");
}

static void test_pause_resume_safety() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);

    snes_pio_pause();
    test_require(!sdk_test::pio[PIO0_INDEX].enabled[CONTROL_SM] &&
                     !sdk_test::pio[PIO0_INDEX].enabled[WRITE_ADDRESS_SM] &&
                     !sdk_test::pio[PIO1_INDEX].enabled[WRITE_TRIGGER_SM] &&
                     !sdk_test::pio[PIO1_INDEX].enabled[WRITE_CAPTURE_SM] &&
                     sdk_test::pio[PIO1_INDEX].enabled[RESET_SM] &&
                     !sdk_test::pio[PIO2_INDEX].enabled[READ_SM],
                 "PIO pause left a transaction state machine enabled");
    require_control_word(SNES_CONTROL_BUS_ISOLATED,
                         "PIO pause did not isolate address/data and disable ROM");
    require_translated_strobes_input("PIO pause drove /I_RD or /I_WR");

    snes_pio_resume();
    require_control_word(SNES_CONTROL_CONSOLE_IDLE,
                         "PIO resume did not restore the listening state");
    test_require(sdk_test::pio[PIO0_INDEX].enabled[CONTROL_SM] &&
                     sdk_test::pio[PIO1_INDEX].enabled[WRITE_CAPTURE_SM] &&
                     sdk_test::pio[PIO2_INDEX].enabled[READ_SM],
                 "PIO resume did not restart transaction state machines");
}

static void test_console_presence_and_local_reset() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);

    sdk_test::set_gpio_level(SNES_PRES_N_PIN, true);
    snes_bus_service();
    test_require(snes_bus_usb_mode(),
                 "deasserted /SNES_PRES did not select USB mode");
    require_control_word(SNES_CONTROL_STANDALONE,
                         "console removal did not disable C_OE#/A_OE#/D_OE# and ROM strobes");
    test_require(sdk_test::gpio_dir[SNES_I_RESET_N_PIN] &&
                     sdk_test::gpio_level[SNES_I_RESET_N_PIN],
                 "C_OE# disable did not leave I_RST# locally driven inactive");
    require_translated_strobes_input("standalone mode drove /I_RD or /I_WR");
    test_require(!sdk_test::pio[PIO1_INDEX].enabled[RESET_SM] &&
                     !sdk_test::pio[PIO2_INDEX].enabled[READ_SM],
                 "standalone mode left PIO console watchers running");

    sdk_test::set_gpio_level(SNES_PRES_N_PIN, false);
    snes_bus_service();
    test_require(!snes_bus_usb_mode(),
                 "asserted /SNES_PRES did not select SNES mode");
    require_control_word(SNES_CONTROL_CONSOLE_IDLE,
                         "console reconnect did not restore translator listening state");
    test_require(!sdk_test::gpio_dir[SNES_I_RESET_N_PIN],
                 "console reconnect enabled C_OE# before releasing local I_RST#");
    require_translated_strobes_input("console reconnect drove /I_RD or /I_WR");
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
    test_require(fx_sync_core1_service(), "core 1 did not service the queued PIO reset");
    test_require(!fx.running() && fx.state().r[15] == 0,
                 "PIO reset did not propagate to the core");
    test_require(sdk_test::gpio_level[SNES_O_IRQ_N_PIN],
                 "reset completion left /O_IRQ asserted");
}

static void test_post_reset_write_cannot_overtake_reset() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);
    sdk_test::set_pio_interrupt(PIO1_INDEX, 2);
    sdk_test::trigger_irq(sdk_test::pio_irq_num(PIO1_INDEX));
    inject_write(0x00, 0x7038, 0x55);
    snes_bus_service();
    fx_sync_core1_service();
    test_require(fx.state().screen_base == 0x55,
                 "post-reset register write overtook reset and was lost");
}

static void test_o_irq_output() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);
    snes_irq_write(nullptr, true);
    test_require(!sdk_test::gpio_level[SNES_O_IRQ_N_PIN],
                 "assert request did not drive /O_IRQ low");
    snes_irq_write(nullptr, false);
    test_require(sdk_test::gpio_level[SNES_O_IRQ_N_PIN],
                 "release request did not drive /O_IRQ high");
    snes_busy_irq_write(nullptr, true);
    snes_irq_write(nullptr, false);
    test_require(!sdk_test::gpio_level[SNES_O_IRQ_N_PIN], "core acknowledgement cleared storage busy");
    snes_irq_write(nullptr, true);
    snes_busy_irq_write(nullptr, false);
    test_require(!sdk_test::gpio_level[SNES_O_IRQ_N_PIN], "storage completion cleared core IRQ");
    snes_irq_write(nullptr, false);
    test_require(sdk_test::gpio_level[SNES_O_IRQ_N_PIN], "last IRQ owner did not release pin");
}

static void test_extended_page_reachability() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);
    for (auto map : {SnesRomMap::ExLoRom, SnesRomMap::ExHiRom}) {
        snes_pio_set_rom_map(map);
        bool mapped[2048]{};
        bool readable[2048]{};
        for (uint32_t address = 0; address < 16u * 1024u * 1024u; address += 4096u) {
            uint32_t source = 0;
            if (!snes_rom_source_offset({map, 8u * 1024u * 1024u, 0}, address, source))
                continue;
            mapped[source / 4096u] = true;
            if (inject_read_word(static_cast<uint8_t>(address >> 16),
                                  static_cast<uint16_t>(address), true) == 0)
                readable[source / 4096u] = true;
        }
        for (unsigned page = 0; page < 2048; ++page)
            test_require(!mapped[page] || readable[page], "mapped extended source page has no readable alias");
    }
}

static void test_canonical_fx3_overlay() {
    TestMemory memory{};
    SuperFx fx;
    init_bus(fx, memory, fx3_config);
    snes_pio_set_rom_map(SnesRomMap::Fx3);
    for (uint8_t bank : {uint8_t{0x70}, uint8_t{0x71}}) {
        inject_write(bank, 0xA123, 0x5A);
        test_require(inject_read(bank, 0xA123) == 0x5A &&
                         inject_read_word(bank, 0xA123, true) != 0,
                     "canonical FX3 ROM displaced SRAM overlay");
    }
    for (uint8_t bank : {uint8_t{0xC0}, uint8_t{0xE0}, uint8_t{0xF0}, uint8_t{0xFF}})
        test_require(inject_read_word(bank, 0xA123, true) == 0,
                     "canonical FX3 upper CPU ROM window is not accessible");
    test_require(inject_read(0, 0x703B) == 0x52, "canonical FX3 register layout changed");
}

int main() {
    test_routed_packers();
    test_fx3_frontend_round_trip();
    test_noncartridge_reads_do_not_drive();
    test_dma_bridges();
    test_fx3_never_steals_parallel_rom_bus();
    test_legacy_physical_rom_transaction();
    test_pause_resume_safety();
    test_console_presence_and_local_reset();
    test_pio_reset_reaches_core1();
    test_post_reset_write_cannot_overtake_reset();
    test_o_irq_output();
    test_extended_page_reachability();
    test_canonical_fx3_overlay();
    std::puts("bus_integration_tests: PASS");
    return 0;
}
