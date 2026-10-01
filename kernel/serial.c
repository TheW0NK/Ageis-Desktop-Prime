#include "serial.h"
#include "cpu.h"

#define COM1    0x3F8

static bool present;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);

    outb(COM1 + 4, 0x1E);
    outb(COM1 + 0, 0xAE);
    present = inb(COM1 + 0) == 0xAE;
    outb(COM1 + 4, 0x0F);
}

void serial_putc(char c)
{
    if (!present)
        return;
    if (c == '\n')
        serial_putc('\r');
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s, size_t len)
{
    while (len--)
        serial_putc(*s++);
}
