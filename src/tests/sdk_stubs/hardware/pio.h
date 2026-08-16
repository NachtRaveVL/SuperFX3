#pragma once
#include <cstdint>
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
inline int pio_claim_unused_sm(PIO, bool) { return 0; }
inline int pio_add_program(PIO, const pio_program_t*) { return 0; }
inline int pio_sm_init(PIO, uint, uint, const pio_sm_config*) { return 0; }
inline int pio_set_gpio_base(PIO, uint) { return 0; }
inline void pio_sm_set_enabled(PIO, uint, bool) {}
inline void pio_sm_clear_fifos(PIO, uint) {}
inline void pio_sm_set_pindirs_with_mask64(PIO, uint, uint64_t, uint64_t) {}
inline void pio_sm_set_pins_with_mask64(PIO, uint, uint64_t, uint64_t) {}
inline void pio_sm_exec(PIO, uint, uint32_t) {}
inline void pio_sm_put(PIO, uint, uint32_t) {}
inline uint32_t pio_sm_get(PIO, uint) { return 0; }
inline bool pio_interrupt_get(PIO, uint) { return false; }
inline void pio_interrupt_clear(PIO, uint) {}
inline void pio_set_irq0_source_enabled(PIO, pio_interrupt_source, bool) {}
inline uint pio_get_irq_num(PIO, uint) { return 0; }
inline uint pio_get_dreq(PIO, uint, bool) { return 0; }
inline void pio_gpio_init(PIO, uint) {}
inline void pio_set_input_sync_bypass_with_mask64(PIO, uint64_t, uint64_t) {}
inline void sm_config_set_in_pins(pio_sm_config*, uint) {}
inline void sm_config_set_set_pins(pio_sm_config*, uint, uint) {}
inline void sm_config_set_in_shift(pio_sm_config*, bool, bool, uint) {}
inline void sm_config_set_fifo_join(pio_sm_config*, pio_fifo_join) {}
inline void sm_config_set_jmp_pin(pio_sm_config*, uint) {}
inline void sm_config_set_out_shift(pio_sm_config*, bool, bool, uint) {}
inline void sm_config_set_out_pins(pio_sm_config*, uint, uint) {}
inline void sm_config_set_sideset_pins(pio_sm_config*, uint) {}
