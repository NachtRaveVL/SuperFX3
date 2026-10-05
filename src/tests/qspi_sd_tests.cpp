#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <array>

#include "pico.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "shared_qspi.h"
#include "test_flash.h"
#include "test_support.h"
#include "../audio/fx3_audio_stream.h"
#include "../platform/rp2350/fx_sync.h"
#include "../platform/rp2350/qspi_bus.h"
#include "../platform/rp2350/qspi_sd.h"
#include "../platform/rp2350/qspi_save.h"
#include "../platform/rp2350/snes_bus.h"
#include "../platform/rp2350/snes_bus_layout.h"
#include "../platform/rp2350/snes_pio.h"

namespace {
SuperFx fx;
bool injected = false;
unsigned action = 0;
unsigned half_cycles = 0;
std::array<uint8_t, 600> card_bytes;
std::array<uint8_t, 600> sent_bytes;
uint32_t card_bits = 0;
bool card_second_half = false;
bool cancel_card = false;
bool timeout_card = false;

void card_bit() {
    if (gpio_get(SNES_SD_CS_N_PIN)) return;
    test_require((qmi_hw->direct_csr & QMI_DIRECT_CSR_EN_BITS) &&
                     (io_qspi_hw->io[1].ctrl & 0x3000u) == 0x3000u,
                 "SD command selected flash during the data phase");
    const uint32_t byte = card_bits / 8u;
    const uint32_t bit = 7u - card_bits % 8u;
    if (!card_second_half) {
        const uint8_t value = byte < card_bytes.size() ? card_bytes[byte] : uint8_t{0xFF};
        sio_hw->gpio_hi_in = (value & (1u << bit)) ? 1u << 29 : 0;
        if (byte < sent_bytes.size() && (sio_hw->gpio_hi_out & (1u << 28)))
            sent_bytes[byte] |= static_cast<uint8_t>(1u << bit);
        if (cancel_card && byte == 24) qspi_bus_audio_cancel();
        if (timeout_card && byte == 24) timer0_hw->timerawl.offset += 25001u;
    } else ++card_bits;
    card_second_half = !card_second_half;
}

void test_command_transaction() {
    const uint8_t packet[6] {0x51, 0, 0, 0, 42, 0xFF};
    uint8_t response[5] {}, block[514] {};
    for (unsigned mode = 0; mode < 6; ++mode) {
        card_bytes.fill(0xFF); sent_bytes.fill(0);
        card_bits = 0; card_second_half = false; cancel_card = mode == 4;
        timeout_card = mode == 5;
        if (mode != 3) card_bytes[10] = 0; // R1 after the command and NCR clocks.
        if (mode == 1) {
            for (unsigned i = 0; i < 4; ++i) card_bytes[11 + i] = static_cast<uint8_t>(0xA0 + i);
        } else if (mode != 5) {
            card_bytes[18] = mode == 2 ? 0x0B : 0xFE;
            for (unsigned i = 0; i < 514; ++i) card_bytes[19 + i] = static_cast<uint8_t>(i);
        }
        sdk_test::busy_wait_hook = card_bit;
        const QspiSdResult result = qspi_sd_command(packet, response, mode == 1 ? 5u : 1u,
                                                    mode == 1 ? nullptr : block, mode == 1 ? 0u : 512u);
        sdk_test::busy_wait_hook = nullptr;
        const QspiSdResult expected = mode < 2 ? QspiSdResult::Ok :
            mode == 2 ? QspiSdResult::ProtocolError :
            mode == 3 || mode == 5 ? QspiSdResult::Timeout : QspiSdResult::Cancelled;
        test_require(result == expected && gpio_get(SNES_SD_CS_N_PIN) && qmi_hw->direct_csr == 0,
                     "SD command/data failure left CS or XIP in the wrong state");
        test_require(std::equal(packet, packet + 6, sent_bytes.begin() + 1),
                     "SD command was not transmitted MSB-first under one CS");
        if (!mode) {
            test_require(card_bits == 533u * 8u, "SD released CS before the payload and CRC ended");
            for (unsigned i = 0; i < 514; ++i)
                test_require(block[i] == static_cast<uint8_t>(i), "SD token polling shifted the data block");
        } else if (mode == 1)
            test_require(response[0] == 0 && response[1] == 0xA0 && response[4] == 0xA3,
                         "SD extended response bytes were lost");
    }
}

void write(uint8_t bank, uint16_t addr, uint8_t value) {
    const uint32_t address = (static_cast<uint32_t>(bank) << 16) | addr;
    sdk_test::set_pio_rx(1, 1, (snes_pack_address_raw(address) << 8) | snes_pack_data_raw(value));
    sdk_test::set_pio_interrupt(1, 1);
    sdk_test::trigger_irq(sdk_test::pio_irq_num(1));
}

uint8_t read(uint8_t bank, uint16_t addr) {
    sdk_test::set_gpio_mask(SNES_ADDR_MASK,
        snes_address_to_gpio((static_cast<uint32_t>(bank) << 16) | addr));
    sdk_test::set_pio_interrupt(2, 0);
    sdk_test::trigger_irq(sdk_test::pio_irq_num(2));
    return snes_unpack_data_raw(static_cast<uint8_t>((sdk_test::pio[2].tx[0] >> 1) & 0xFFu));
}

void check_live_service() {
    ++half_cycles;
    test_require((qmi_hw->direct_csr & QMI_DIRECT_CSR_EN_BITS) &&
                     (io_qspi_hw->io[1].ctrl & 0x3000u) == 0x3000u &&
                     sdk_test::irq_enabled[sdk_test::pio_irq_num(1)] &&
                     sdk_test::irq_enabled[sdk_test::pio_irq_num(2)] &&
                     !sdk_test::irq_enabled[15] && systick_hw->csr == 0 &&
                     scb_hw->icsr == 0,
                 "SD ran without isolated flash and live PIO-only IRQ service");
    if (injected || gpio_get(SNES_SD_CS_N_PIN))
        return;
    injected = true;
    test_require(!qspi_save_now(nullptr) && sdk_flash::programs == 0,
                 "QSPI save entered during an SD transaction");
    write(0x70, 42, 0xA5);
    test_require(read(0x70, 42) == 0xA5, "live SD window stopped serving shared SRAM");
    write(0x00, 0x7038, 0x55);
    test_require(fx.state().screen_base != 0x55, "GSU register write executed while XIP was unavailable");
    test_require(read(0x00, 0x6FFF) == 0, "live SD window lost the permanent empty HDMA table");
    if (action == 1)
        qspi_bus_audio_cancel();
    else if (action == 2) {
        for (unsigned i = 0; i < 128; ++i)
            test_require(fx_sync_sd_cpu_write(0x7038, static_cast<uint8_t>(i)),
                         "SD write queue filled before the cancellation watermark");
    } else if (action == 3) {
        sdk_test::set_pio_interrupt(1, 2);
        sdk_test::trigger_irq(sdk_test::pio_irq_num(1));
        write(0x00, 0x7038, 0x66);
    } else if (action == 4)
        timer0_hw->timerawl.offset += 25001u;
}

void test_live_sd_and_cleanup() {
    TestMemory memory;
    sdk_test::reset_hardware();
    const FxBackend backend = make_test_backend(memory);
    fx.init(fx3_config, backend);
    fx_sync_init(fx, backend);
    fx3_audio_init({});
    snes_bus_init();
    snes_pio_set_rom_map(SnesRomMap::Fx3);
    sdk_test::set_gpio_level(SNES_PRES_PIN, true);
    sdk_test::set_gpio_level(SNES_I_RESET_N_PIN, true);
    snes_bus_start(fx);
    sdk_test::irq_enabled[15] = true;
    systick_hw->csr = 7;
    scb_hw->icsr = M33_ICSR_PENDSTSET_BITS | M33_ICSR_PENDSVSET_BITS;
    sio_hw->gpio_hi_in = 1u << 29;
    sio_hw->gpio_hi_out = 0x12345678;
    sio_hw->gpio_hi_oe = 0x87654321;
    for (unsigned i = 0; i < 6; ++i) {
        io_qspi_hw->io[i].ctrl = 0x10000u + i;
        pads_qspi_hw->io[i] = 0x50u + i;
    }
    static std::atomic<uint8_t> ram[128u * 1024u] {};
    sdk_flash::bytes.fill(0xFF);
    qspi_save_init(ram);
    const auto read_handler = sdk_test::irq_handler[sdk_test::pio_irq_num(2)];
    const auto control_handler = sdk_test::irq_handler[sdk_test::pio_irq_num(1)];
    std::atomic<bool> done {false};
    std::thread core0([&] {
        while (!done.load(std::memory_order_acquire)) {
            qspi_bus_core0_service();
            std::this_thread::yield();
        }
    });
    uint8_t tx[512], rx[512];
    std::fill_n(tx, sizeof(tx), 0xAA);
    for (action = 0; action < 5; ++action) {
        fx.cpu_write(0x7038, 0x40);
        injected = false;
        half_cycles = 0;
        sdk_test::busy_wait_hook = check_live_service;
        const QspiSdResult result = qspi_sd_exchange(tx, rx, sizeof(tx));
        sdk_test::busy_wait_hook = nullptr;
        test_require(result == (action == 0 ? QspiSdResult::Ok :
                                action == 4 ? QspiSdResult::Timeout : QspiSdResult::Cancelled),
                     "SD cancellation/timeout did not report the correct result");
        test_require(injected && half_cycles >= 16 && gpio_get(SNES_SD_CS_N_PIN) &&
                         qmi_hw->direct_csr == 0 && systick_hw->csr == 7 &&
                         scb_hw->icsr == (M33_ICSR_PENDSTSET_BITS | M33_ICSR_PENDSVSET_BITS) &&
                         sdk_test::irq_enabled[15] &&
                         sdk_test::irq_handler[sdk_test::pio_irq_num(2)] == read_handler &&
                         sdk_test::irq_handler[sdk_test::pio_irq_num(1)] == control_handler &&
                         sio_hw->gpio_hi_out == 0x12345678 && sio_hw->gpio_hi_oe == 0x87654321,
                     "SD completion failed to restore pins, XIP, vectors or IRQs");
        for (unsigned i = 0; i < 6; ++i)
            test_require(io_qspi_hw->io[i].ctrl == 0x10000u + i &&
                             pads_qspi_hw->io[i] == 0x50u + i,
                         "SD did not restore the QSPI pad/mux configuration");
        test_require(fx.state().screen_base == (action == 2 ? 127 : action == 3 ? 0x66 : 0x55),
                     "deferred writes/reset were not applied in order after restoring XIP");
        if (action == 0)
            test_require(std::all_of(rx, rx + sizeof(rx), [](uint8_t value) { return value == 0xFF; }),
                         "SPI did not sample MISO on every transferred bit");
    }
    sdk_test::busy_wait_hook = nullptr;
    test_command_transaction();
    const uint32_t assertions = sdk_test::gpio_low_count[SNES_SD_CS_N_PIN];
    test_require(qspi_bus_try_acquire(), "SD leaked the shared QSPI owner");
    test_require(qspi_sd_exchange(tx, rx, sizeof(tx)) == QspiSdResult::Busy &&
                     sdk_test::gpio_low_count[SNES_SD_CS_N_PIN] == assertions,
                 "SD entered while another QSPI owner was active");
    qspi_bus_release();
    qmi_hw->direct_csr.busy = true;
    test_require(qspi_sd_exchange(tx, rx, 1) == QspiSdResult::Timeout &&
                     sdk_test::gpio_low_count[SNES_SD_CS_N_PIN] == assertions,
                 "SD asserted CS before an outstanding flash transfer retired");
    qmi_hw->direct_csr.busy = false;
    sio_hw->cpuid = 0;
    test_require(qspi_sd_exchange(tx, rx, 1) == QspiSdResult::Unavailable,
                 "Core 0 entered the Core-1 SD transaction path");
    sio_hw->cpuid = 1;
    test_require(qspi_sd_exchange(tx, rx, 0) == QspiSdResult::Unavailable &&
                     qspi_sd_exchange(tx, rx, 4097) == QspiSdResult::Unavailable &&
                     qspi_sd_exchange(tx, rx, 1, 100000) == QspiSdResult::Unavailable,
                 "SD accepted an invalid burst/clock limit");
    done.store(true, std::memory_order_release);
    core0.join();
    test_require(qspi_sd_exchange(tx, rx, 1) == QspiSdResult::Busy && qspi_bus_try_acquire(),
                 "failed Core-0 acknowledgement left SD selected or ownership locked");
    qspi_bus_release();
    ram[99].store(0xA5);
    test_require(qspi_save_now(nullptr) && sdk_flash::programs != 0,
                 "save writer did not regain ownership after SD cleanup");
}
}

int main() {
    test_live_sd_and_cleanup();
    std::puts("qspi_sd_tests: PASS");
}
