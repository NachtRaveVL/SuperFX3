#pragma once

#include <array>
#include <cstdint>

namespace sdk_test {

static constexpr unsigned GPIO_COUNT = 64;
static constexpr unsigned PIO_COUNT = 3;
static constexpr unsigned SM_COUNT = 4;
static constexpr unsigned PIO_IRQ_COUNT = 8;
static constexpr unsigned IRQ_COUNT = 16;
static constexpr unsigned DMA_COUNT = 16;

struct PioState {
    unsigned next_sm = 0;
    unsigned next_offset = 0;
    unsigned gpio_base = 0;
    std::array<bool, SM_COUNT> enabled{};
    std::array<bool, SM_COUNT> initialized{};
    std::array<uint32_t, SM_COUNT> rx{};
    std::array<uint32_t, SM_COUNT> tx{};
    std::array<uint32_t, SM_COUNT> osr{};
    std::array<uint32_t, SM_COUNT> x{};
    std::array<uint32_t, SM_COUNT> last_exec{};
    std::array<uint64_t, SM_COUNT> last_pins{};
    std::array<uint64_t, SM_COUNT> last_pin_mask{};
    std::array<uint64_t, SM_COUNT> last_pindirs{};
    std::array<uint64_t, SM_COUNT> last_pindir_mask{};
    std::array<bool, PIO_IRQ_COUNT> interrupts{};
    std::array<bool, PIO_IRQ_COUNT> irq0_source_enabled{};
};

struct DmaState {
    bool configured = false;
    bool running = false;
    bool aborted = false;
};

inline std::array<bool, GPIO_COUNT> gpio_level{};
inline std::array<bool, GPIO_COUNT> gpio_dir{};
inline std::array<bool, GPIO_COUNT> gpio_pullup{};
inline std::array<int, GPIO_COUNT> gpio_function{};
inline std::array<uint32_t, GPIO_COUNT> gpio_put_count{};
inline std::array<uint32_t, GPIO_COUNT> gpio_low_count{};
inline std::array<PioState, PIO_COUNT> pio{};
inline std::array<void (*)(), IRQ_COUNT> irq_handler{};
inline std::array<bool, IRQ_COUNT> irq_enabled{};
inline std::array<DmaState, DMA_COUNT> dma{};
inline unsigned next_dma = 0;
inline uint64_t busy_wait_cycles = 0;
inline void (*tight_loop_hook)() = nullptr;

inline void reset_hardware() {
    gpio_level.fill(false);
    gpio_dir.fill(false);
    gpio_pullup.fill(false);
    gpio_function.fill(-1);
    gpio_put_count.fill(0);
    gpio_low_count.fill(0);
    pio.fill(PioState{});
    irq_handler.fill(nullptr);
    irq_enabled.fill(false);
    dma.fill(DmaState{});
    next_dma = 0;
    busy_wait_cycles = 0;
    tight_loop_hook = nullptr;
}

inline void set_gpio_level(unsigned pin, bool value) {
    gpio_level.at(pin) = value;
}

inline void set_gpio_mask(uint64_t mask, uint64_t values) {
    for (unsigned pin = 0; pin < GPIO_COUNT; ++pin) {
        const uint64_t bit = uint64_t{1} << pin;
        if (mask & bit)
            gpio_level[pin] = (values & bit) != 0;
    }
}

inline uint64_t gpio_snapshot() {
    uint64_t value = 0;
    for (unsigned pin = 0; pin < GPIO_COUNT; ++pin) {
        if (gpio_level[pin])
            value |= uint64_t{1} << pin;
    }
    return value;
}

inline void set_pio_rx(unsigned pio_index, unsigned sm, uint32_t value) {
    pio.at(pio_index).rx.at(sm) = value;
}

inline void set_pio_interrupt(unsigned pio_index, unsigned interrupt, bool value = true) {
    pio.at(pio_index).interrupts.at(interrupt) = value;
}

inline unsigned pio_irq_num(unsigned pio_index, unsigned irq_index = 0) {
    return pio_index * 2u + irq_index;
}

inline void trigger_irq(unsigned irq) {
    if (irq_enabled.at(irq) && irq_handler.at(irq))
        irq_handler[irq]();
}

} // namespace sdk_test
