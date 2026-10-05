#pragma once

#ifndef __not_in_flash
#define __not_in_flash(group) __attribute__((section(".time_critical." group)))
#endif

#define __no_inline_not_in_flash_func(func_name) __attribute__((noinline)) __not_in_flash_func(func_name)

#ifndef __not_in_flash_func
#define __not_in_flash_func(func_name) __not_in_flash(__STRING(func_name)) func_name
#endif
