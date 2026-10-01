#include "crc.h"

static uint32_t crc32c_table[256];
static uint16_t crc16_table[256];
static bool ready;

static void init_tables(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        uint16_t c16 = i;

        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (c >> 1) ^ 0x82F63B78 : c >> 1;
            c16 = (c16 & 1) ? (c16 >> 1) ^ 0xA001 : c16 >> 1;
        }
        crc32c_table[i] = c;
        crc16_table[i] = c16;
    }
    ready = true;
}

uint32_t crc32c(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;

    if (!ready)
        init_tables();
    while (len--)
        crc = crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint16_t crc16(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;

    if (!ready)
        init_tables();
    while (len--)
        crc = crc16_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}
