#include "keyboard.h"
#include "apic.h"
#include "cpu.h"
#include "string.h"
#include "tty.h"

#define KBD_DATA        0x60
#define KBD_STATUS      0x64
#define KBD_COMMAND     0x64

static const char normal[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ',
};

static const char shifted[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ',
};

static bool shift, ctrl, caps, extended, present;

static void push(char c)
{
    tty_input(&c, 1);
}

static void push_seq(const char *seq)
{
    tty_input(seq, strlen(seq));
}

static void handle(uint8_t sc)
{
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;
    char c;

    if (sc == 0xE0) {
        extended = true;
        return;
    }
    if (extended) {
        extended = false;
        if (code == 0x1D) {
            ctrl = !release;
            return;
        }
        if (release)
            return;
        switch (code) {
        case 0x48: push_seq("\x1b[A"); break;
        case 0x50: push_seq("\x1b[B"); break;
        case 0x4D: push_seq("\x1b[C"); break;
        case 0x4B: push_seq("\x1b[D"); break;
        case 0x47: push_seq("\x1b[H"); break;
        case 0x4F: push_seq("\x1b[F"); break;
        case 0x53: push_seq("\x1b[3~"); break;
        case 0x1C: push('\n'); break;
        case 0x35: push('/'); break;
        }
        return;
    }

    switch (code) {
    case 0x2A:
    case 0x36:
        shift = !release;
        return;
    case 0x1D:
        ctrl = !release;
        return;
    case 0x3A:
        if (!release)
            caps = !caps;
        return;
    }
    if (release)
        return;

    c = shift ? shifted[code] : normal[code];
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c &= 0x1F;
    if (c)
        push(c);
}

static void keyboard_irq(struct interrupt_frame *frame)
{
    (void)frame;
    while (inb(KBD_STATUS) & 1)
        handle(inb(KBD_DATA));
}

static void wait_write(void)
{
    for (int i = 0; i < 100000 && (inb(KBD_STATUS) & 2); i++)
        ;
}

static bool wait_read(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(KBD_STATUS) & 1)
            return true;
    }
    return false;
}

void keyboard_init(void)
{
    uint8_t config;

    if (inb(KBD_STATUS) == 0xFF)
        return;

    while (inb(KBD_STATUS) & 1)
        inb(KBD_DATA);

    wait_write();
    outb(KBD_COMMAND, 0x20);
    if (!wait_read())
        return;
    config = inb(KBD_DATA) | 0x01 | 0x40;
    wait_write();
    outb(KBD_COMMAND, 0x60);
    wait_write();
    outb(KBD_DATA, config);
    wait_write();
    outb(KBD_COMMAND, 0xAE);

    present = irq_install_isa(1, keyboard_irq) >= 0;
}

bool keyboard_present(void)
{
    return present;
}

