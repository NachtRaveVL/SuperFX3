#pragma once

#ifndef __not_in_flash
#define __not_in_flash(group) __attribute__((section(".time_critical." group)))
#endif

#ifndef __not_in_flash_func
#define __not_in_flash_func(func_name) __not_in_flash(__STRING(func_name)) func_name
#endif
