#pragma once
#include <cstdint>
#include "../test_hardware.h"
#include "pio_instructions.h"
#define NUM_PIOS 3
#ifndef PICO_PIO_USE_GPIO_BASE
#define PICO_PIO_USE_GPIO_BASE 1
#endif
using uint = unsigned int;
struct pio_sm_config {};
struct pio_program_t {};
struct pio_hw_t { uint32_t txf[4]{}; uint32_t rxf[4]{}; };
using PIO = pio_hw_t*;
inline pio_hw_t pio0_hw{}, pio1_hw{}, pio2_hw{};
inline PIO pio0=&pio0_hw, pio1=&pio1_hw, pio2=&pio2_hw;
enum pio_fifo_join { PIO_FIFO_JOIN_RX };
enum pio_interrupt_source { pis_interrupt0, pis_interrupt1, pis_interrupt2 };

inline unsigned pio_test_index(PIO pio) {
    if (pio == pio0) return 0;
    if (pio == pio1) return 1;
    return 2;
}
inline int pio_claim_unused_sm(PIO pio, bool) {
    auto& state = sdk_test::pio.at(pio_test_index(pio));
    if (state.next_sm >= sdk_test::SM_COUNT)
        return -1;
    return static_cast<int>(state.next_sm++);
}
inline int pio_add_program(PIO pio, const pio_program_t*) {
    auto& state = sdk_test::pio.at(pio_test_index(pio));
    return static_cast<int>(state.next_offset++);
}
inline int pio_sm_init(PIO pio, uint sm, uint, const pio_sm_config*) {
    sdk_test::pio.at(pio_test_index(pio)).initialized.at(sm) = true;
    return 0;
}
inline int pio_set_gpio_base(PIO pio, uint base) {
    sdk_test::pio.at(pio_test_index(pio)).gpio_base = base;
    return 0;
}
inline void pio_sm_set_enabled(PIO pio, uint sm, bool enabled) {
    sdk_test::pio.at(pio_test_index(pio)).enabled.at(sm) = enabled;
}
inline void pio_sm_clear_fifos(PIO pio, uint sm) {
    auto& state = sdk_test::pio.at(pio_test_index(pio));
    state.rx.at(sm) = 0;
    state.tx.at(sm) = 0;
    state.osr.at(sm) = 0;
}
inline void pio_sm_set_pindirs_with_mask64(PIO pio, uint sm, uint64_t values, uint64_t mask) {
    auto& state = sdk_test::pio.at(pio_test_index(pio));
    state.last_pindirs.at(sm) = values;
    state.last_pindir_mask.at(sm) = mask;
    for (uint pin = 0; pin < sdk_test::GPIO_COUNT; ++pin) {
        const uint64_t bit = uint64_t{1} << pin;
        if (mask & bit)
            sdk_test::gpio_dir[pin] = (values & bit) != 0;
    }
}
inline void pio_sm_set_pins_with_mask64(PIO pio, uint sm, uint64_t values, uint64_t mask) {
    auto& state = sdk_test::pio.at(pio_test_index(pio));
    state.last_pins.at(sm) = values;
    state.last_pin_mask.at(sm) = mask;
    sdk_test::set_gpio_mask(mask, values);
}
inline void pio_sm_exec(PIO pio, uint sm, uint32_t instruction) {
    auto& state = sdk_test::pio.at(pio_test_index(pio));
    state.last_exec.at(sm) = instruction;
    const uint32_t tag = instruction & 0xF0000000u;
    if (tag == PIO_TEST_SET_TAG) {
        const uint32_t dest = (instruction >> 8) & 0xFFu;
        if (dest == static_cast<uint32_t>(pio_x))
            state.x.at(sm) = instruction & 0x1Fu;
    } else if (tag == PIO_TEST_PULL_TAG) {
        state.osr.at(sm) = state.tx.at(sm);
    } else if (tag == PIO_TEST_MOV_TAG) {
        const uint32_t dest = (instruction >> 8) & 0xFFu;
        const uint32_t src = instruction & 0xFFu;
        if (dest == static_cast<uint32_t>(pio_x) && src == static_cast<uint32_t>(pio_osr))
            state.x.at(sm) = state.osr.at(sm);
    }
}
inline void pio_sm_put(PIO pio, uint sm, uint32_t value) {
    auto& state = sdk_test::pio.at(pio_test_index(pio));
    state.tx.at(sm) = value;
    pio->txf[sm] = value;
}
inline uint32_t pio_sm_get(PIO pio, uint sm) {
    return sdk_test::pio.at(pio_test_index(pio)).rx.at(sm);
}
inline bool pio_interrupt_get(PIO pio, uint interrupt) {
    return sdk_test::pio.at(pio_test_index(pio)).interrupts.at(interrupt);
}
inline void pio_interrupt_clear(PIO pio, uint interrupt) {
    sdk_test::pio.at(pio_test_index(pio)).interrupts.at(interrupt) = false;
}
inline void pio_set_irq0_source_enabled(PIO pio, pio_interrupt_source source, bool enabled) {
    sdk_test::pio.at(pio_test_index(pio)).irq0_source_enabled.at(static_cast<unsigned>(source)) = enabled;
}
inline uint pio_get_irq_num(PIO pio, uint irq_index) {
    return sdk_test::pio_irq_num(pio_test_index(pio), irq_index);
}
inline uint pio_get_dreq(PIO pio, uint sm, bool is_tx) {
    return pio_test_index(pio) * 8u + sm * 2u + (is_tx ? 1u : 0u);
}
inline void pio_gpio_init(PIO pio, uint pin) {
    sdk_test::gpio_function.at(pin) = 100 + static_cast<int>(pio_test_index(pio));
}
inline void pio_set_input_sync_bypass_with_mask64(PIO, uint64_t, uint64_t) {}
inline void sm_config_set_in_pins(pio_sm_config*, uint) {}
inline void sm_config_set_set_pins(pio_sm_config*, uint, uint) {}
inline void sm_config_set_in_shift(pio_sm_config*, bool, bool, uint) {}
inline void sm_config_set_fifo_join(pio_sm_config*, pio_fifo_join) {}
inline void sm_config_set_jmp_pin(pio_sm_config*, uint) {}
inline void sm_config_set_out_shift(pio_sm_config*, bool, bool, uint) {}
inline void sm_config_set_out_pins(pio_sm_config*, uint, uint) {}
inline void sm_config_set_sideset_pins(pio_sm_config*, uint) {}
