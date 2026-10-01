#ifndef AEGIS_RANDOM_H
#define AEGIS_RANDOM_H

#include "kernel.h"

void random_init(void);
void random_mix(uint64_t data);
void random_bytes(void *buf, size_t n);
uint64_t random_u64(void);
void chacha20_block(const uint32_t key[8], uint64_t counter, const uint32_t nonce[2], uint32_t out[16]);

#endif
