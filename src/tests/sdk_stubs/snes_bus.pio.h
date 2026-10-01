#pragma once
#include "hardware/pio.h"
#define DECLARE_PIO(name) \
    inline const pio_program_t name##_program{}; \
    inline pio_sm_config name##_program_get_default_config(uint) { return {}; }
DECLARE_PIO(snes_control_output)
DECLARE_PIO(snes_write_address)
DECLARE_PIO(snes_write_trigger)
DECLARE_PIO(snes_write_capture)
DECLARE_PIO(snes_reset)
DECLARE_PIO(snes_read)
#undef DECLARE_PIO
