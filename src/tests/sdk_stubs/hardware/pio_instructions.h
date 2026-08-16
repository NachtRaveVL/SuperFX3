#pragma once
#include <cstdint>
enum pio_src_dest { pio_x };
inline uint32_t pio_encode_set(pio_src_dest, uint32_t) { return 0; }
