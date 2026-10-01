#ifndef AEGIS_CRC_H
#define AEGIS_CRC_H

#include "kernel.h"

// Raw CRCs: no implicit pre- or post-inversion, as ext4 and jbd2 expect.
uint32_t crc32c(uint32_t crc, const void *data, size_t len);
uint16_t crc16(uint16_t crc, const void *data, size_t len);

#endif
