#ifndef AEGIS_SHA256_H
#define AEGIS_SHA256_H

#include "kernel.h"

struct sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);
void sha256_final(struct sha256 *s, uint8_t out[32]);

#endif
