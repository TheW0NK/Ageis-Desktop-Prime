#ifndef _ATOMIC_H
#define _ATOMIC_H
// Aegis: only the bit helpers musl's fma.c uses.
#include <stdint.h>
static inline int a_clz_64(uint64_t x) { return __builtin_clzll(x); }
static inline int a_ctz_64(uint64_t x) { return __builtin_ctzll(x); }
#endif
