#pragma once
#include "hardware/pio.h"
#define DECLARE_PIO(name) \
    inline const pio_program_t name##_program{}; \
    inline pio_sm_config name##_program_get_default_config(uint) { return {}; }
DECLARE_PIO(snes_select_fx3)
DECLARE_PIO(snes_select_gsu)
DECLARE_PIO(snes_write_addr)
DECLARE_PIO(snes_write_fx3)
DECLARE_PIO(snes_write_gsu)
DECLARE_PIO(snes_reset)
DECLARE_PIO(snes_read_fx3)
DECLARE_PIO(snes_read_fx3_dual)
DECLARE_PIO(snes_read_gsu)
DECLARE_PIO(snes_read_gsu_dual)
#undef DECLARE_PIO
