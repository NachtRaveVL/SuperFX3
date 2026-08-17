#pragma once
#include <cstdint>
#include <cstdlib>
enum pio_src_dest { pio_x = 1, pio_y = 2, pio_osr = 7 };
static constexpr uint32_t PIO_TEST_SET_TAG = 0x10000000u;
static constexpr uint32_t PIO_TEST_PULL_TAG = 0x20000000u;
static constexpr uint32_t PIO_TEST_MOV_TAG = 0x30000000u;
inline uint32_t pio_encode_set(pio_src_dest dest, uint32_t value) {
    if (value > 31)
        std::abort();

    return PIO_TEST_SET_TAG | (static_cast<uint32_t>(dest) << 8) | value;
}
inline uint32_t pio_encode_pull(bool if_empty, bool block) {
    return PIO_TEST_PULL_TAG | (if_empty ? 2u : 0u) | (block ? 1u : 0u);
}
inline uint32_t pio_encode_mov(pio_src_dest dest, pio_src_dest src) {
    return PIO_TEST_MOV_TAG | (static_cast<uint32_t>(dest) << 8) | static_cast<uint32_t>(src);
}
