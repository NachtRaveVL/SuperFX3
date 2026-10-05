#pragma once

#ifndef __STRING
#define __STRING(x) #x
#endif
#define __force_inline inline __attribute__((always_inline))
#define __compiler_memory_barrier() asm volatile("" ::: "memory")
