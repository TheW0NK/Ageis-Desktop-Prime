#include "keyboard.h"
#include "apic.h"
#include "cpu.h"
#include "input.h"
#include "random.h"
#include "spinlock.h"
#include "string.h"

// PS/2 controller: a keyboard on the first port (scancode set 1, translated
// by the controller) and a mouse on the second (IntelliMouse wheel and
// 5-button protocols when the mouse supports them).

#define PS2_DATA        0x60
#define PS2_STATUS      0x64
#define PS2_COMMAND     0x64

#define STATUS_OUTPUT   0x01
#define STATUS_INPUT    0x02
#define STATUS_AUX      0x20

static spinlock_t lock = SPINLOCK_INIT;
static int kbd_dev = -1, mouse_dev = -1;
static uint8_t prefix;              // 0, 0xE0, or 0xE1 while a Pause sequence arrives
static int pause_skip;
static uint8_t mouse_id, packet[4];
static int packet_len, packet_pos;
static uint8_t mouse_buttons;

// Scancode set 1 to key code, unprefixed.
static const uint16_t set1[0x80] = {
    [0x01] = KEY_ESC,
    [0x02] = KEY_1, KEY_1 + 1, KEY_1 + 2, KEY_1 + 3, KEY_1 + 4, KEY_1 + 5, KEY_1 + 6,
    KEY_1 + 7, KEY_1 + 8, KEY_0,
    [0x0C] = KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB,
    [0x10] = 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13,   // q w e r t y u i o p
    [0x1A] = KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_ENTER, KEY_LEFTCTRL,
    [0x1E] = 0x04, 0x16, 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F,         // a s d f g h j k l
    [0x27] = KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_LEFTSHIFT, KEY_BACKSLASH,
    [0x2C] = 0x1D, 0x1B, 0x06, 0x19, 0x05, 0x11, 0x10,                     // z x c v b n m
    [0x33] = KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_RIGHTSHIFT, KEY_KPASTERISK, KEY_LEFTALT,
    [0x39] = KEY_SPACE, KEY_CAPSLOCK,
    [0x3B] = KEY_F1, KEY_F1 + 1, KEY_F1 + 2, KEY_F1 + 3, KEY_F1 + 4, KEY_F1 + 5, KEY_F1 + 6,
    KEY_F1 + 7, KEY_F1 + 8, KEY_F1 + 9,
    [0x45] = KEY_NUMLOCK, KEY_SCROLLLOCK,
    [0x47] = KEY_KP1 + 6, KEY_KP1 + 7, KEY_KP1 + 8, KEY_KPMINUS,
    [0x4B] = KEY_KP1 + 3, KEY_KP1 + 4, KEY_KP1 + 5, KEY_KPPLUS,
    [0x4F] = KEY_KP1, KEY_KP1 + 1, KEY_KP1 + 2, KEY_KP0, KEY_KPDOT,
    [0x54] = KEY_SYSRQ,
    [0x56] = KEY_102ND, KEY_F1 + 10, KEY_F12, KEY_KPEQUAL,
    // F13-F24: QEMU and Linux use 0x5D-0x5F and 0x55 for F13-F16; the
    // Microsoft scan code table uses 0x64-0x6E and 0x76 for F13-F24.
    [0x5D] = KEY_F13, KEY_F13 + 1, KEY_F13 + 2, [0x55] = KEY_F13 + 3,
    [0x64] = KEY_F13, KEY_F13 + 1, KEY_F13 + 2, KEY_F13 + 3, KEY_F13 + 4, KEY_F13 + 5,
    KEY_F13 + 6, KEY_F13 + 7, KEY_F13 + 8, KEY_F13 + 9, KEY_F13 + 10, [0x76] = KEY_F24,
    [0x70] = KEY_KATAKANAHIRAGANA, [0x73] = KEY_RO, [0x79] = KEY_HENKAN,
    [0x7B] = KEY_MUHENKAN, [0x7D] = KEY_YEN, [0x7E] = KEY_KPCOMMA,
};

// Scancode set 1 to key code, after an 0xE0 prefix.
static const uint16_t set1_e0[0x80] = {
    [0x10] = KEY_PREVIOUSSONG, [0x19] = KEY_NEXTSONG, [0x1C] = KEY_KPENTER,
    [0x1D] = KEY_RIGHTCTRL, [0x20] = KEY_MUTE, [0x21] = KEY_CALC, [0x22] = KEY_PLAYPAUSE,
    [0x24] = KEY_STOPCD, [0x2E] = KEY_VOLUMEDOWN, [0x30] = KEY_VOLUMEUP, [0x32] = KEY_WWW,
    [0x35] = KEY_KPSLASH, [0x37] = KEY_SYSRQ, [0x38] = KEY_RIGHTALT, [0x46] = KEY_PAUSE,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGEUP, [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT, [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGEDOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE, [0x5B] = KEY_LEFTMETA, [0x5C] = KEY_RIGHTMETA,
    [0x5D] = KEY_COMPOSE, [0x5E] = KEY_POWER, [0x5F] = KEY_SLEEP, [0x63] = KEY_WAKEUP,
    [0x65] = KEY_SEARCH, [0x66] = KEY_BOOKMARKS, [0x67] = KEY_REFRESH, [0x69] = KEY_FORWARD,
    [0x6A] = KEY_BACK, [0x6B] = KEY_COMPUTER, [0x6C] = KEY_MAIL, [0x6D] = KEY_MEDIA,
};

static bool wait_input_clear(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & STATUS_INPUT))
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool wait_output(void)
{
    for (int i = 0; i < 200000; i++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT)
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static void command(uint8_t cmd)
{
    wait_input_clear();
    outb(PS2_COMMAND, cmd);
}

static void write_data(uint8_t v)
{
    wait_input_clear();
    outb(PS2_DATA, v);
}

static int read_data(void)
{
    return wait_output() ? inb(PS2_DATA) : -1;
}

// Sends a byte to the mouse and waits for its acknowledgement. Only used
// before the interrupt handlers are installed.
static bool mouse_send(uint8_t v)
{
    for (int tries = 0; tries < 3; tries++) {
        int r;

        command(0xD4);
        write_data(v);
        r = read_data();
        if (r == 0xFA)
            return true;
        if (r != 0xFE)
            return false;
    }
    return false;
}

static bool mouse_set_rate(uint8_t rate)
{
    return mouse_send(0xF3) && mouse_send(rate);
}

static int mouse_get_id(void)
{
    return mouse_send(0xF2) ? read_data() : -1;
}

static void key(uint16_t code, bool release)
{
    if (code)
        input_key(kbd_dev, code, !release);
}

static void keyboard_byte(uint8_t sc)
{
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;

    if (sc == 0xFA || sc == 0xFE || sc == 0x00 || sc == 0xFF)
        return;                     // acknowledgements and errors
    if (pause_skip) {
        // Pause sends E1 1D 45 E1 9D C5 with no release of its own.
        if (--pause_skip == 0 && prefix == 0xE1) {
            key(KEY_PAUSE, false);
            key(KEY_PAUSE, true);
            prefix = 0;
        }
        return;
    }
    if (sc == 0xE0) {
        prefix = 0xE0;
        return;
    }
    if (sc == 0xE1) {
        prefix = 0xE1;
        pause_skip = 5;
        return;
    }
    if (prefix == 0xE0) {
        prefix = 0;
        // Print Screen and others wrap themselves in fake shift presses.
        if (code == 0x2A || code == 0x36)
            return;
        key(set1_e0[code], release);
        return;
    }
    key(set1[code], release);
}

static void mouse_packet(void)
{
    uint8_t b = packet[0];
    int dx = packet[1] - ((b << 4) & 0x100);
    int dy = packet[2] - ((b << 3) & 0x100);
    uint8_t buttons = b & 7;
    int wheel = 0;

    if (b & 0xC0)
        dx = dy = 0;                // overflow: the deltas are meaningless
    if (mouse_id == 3) {
        wheel = (int8_t)packet[3];
    } else if (mouse_id == 4) {
        wheel = (int8_t)(packet[3] << 4) >> 4;
        buttons |= (packet[3] >> 1) & 0x18;
    }

    for (int i = 0; i < 5; i++) {
        static const uint16_t codes[5] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA };
        uint8_t bit = 1 << i;

        if ((buttons ^ mouse_buttons) & bit)
            input_key(mouse_dev, codes[i], buttons & bit);
    }
    mouse_buttons = buttons;
    input_rel(mouse_dev, REL_X, dx);
    input_rel(mouse_dev, REL_Y, -dy);
    input_rel(mouse_dev, REL_WHEEL, -wheel);
    input_sync(mouse_dev);
}

static void mouse_byte(uint8_t v)
{
    if (packet_pos == 0 && !(v & 0x08))
        return;                     // out of sync: wait for a valid first byte
    packet[packet_pos++] = v;
    if (packet_pos == packet_len) {
        packet_pos = 0;
        mouse_packet();
    }
}

static void drain(void)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    uint8_t status;

    while ((status = inb(PS2_STATUS)) & STATUS_OUTPUT) {
        uint8_t v = inb(PS2_DATA);

        random_mix(v);
        if (status & STATUS_AUX) {
            if (mouse_dev >= 0)
                mouse_byte(v);
        } else if (kbd_dev >= 0) {
            keyboard_byte(v);
        }
    }
    spin_unlock_irqrestore(&lock, flags);
}

static void ps2_irq(struct interrupt_frame *frame)
{
    (void)frame;
    drain();
}

static void set_leds(void *ctx, uint32_t mods)
{
    uint8_t leds = 0;

    (void)ctx;
    if (mods & MOD_SCROLLLOCK)
        leds |= 1;
    if (mods & MOD_NUMLOCK)
        leds |= 2;
    if (mods & MOD_CAPSLOCK)
        leds |= 4;
    // The keyboard's acknowledgements arrive through the interrupt handler,
    // which discards them.
    write_data(0xED);
    for (int i = 0; i < 20000; i++)
        __asm__ volatile ("pause");
    write_data(leds);
}

static bool init_mouse(void)
{
    int id;

    if (!mouse_send(0xF6))
        return false;
    // Knock sequences that unlock the wheel, then the extra buttons.
    if (mouse_set_rate(200) && mouse_set_rate(100) && mouse_set_rate(80)
        && (id = mouse_get_id()) == 3) {
        mouse_id = 3;
        if (mouse_set_rate(200) && mouse_set_rate(200) && mouse_set_rate(80)
            && mouse_get_id() == 4)
            mouse_id = 4;
    }
    mouse_set_rate(100);
    packet_len = mouse_id ? 4 : 3;
    return mouse_send(0xF4);
}

static bool kbd_present, mouse_present;

void ps2_init(void)
{
    uint8_t config;
    int r;

    if (inb(PS2_STATUS) == 0xFF)
        return;                     // no controller

    command(0xAD);
    command(0xA7);
    while (inb(PS2_STATUS) & STATUS_OUTPUT)
        inb(PS2_DATA);

    command(0x20);
    if ((r = read_data()) < 0)
        return;
    config = r;
    // Interrupts off while probing; keep scancode translation on.
    config &= ~0x03;
    config |= 0x40;
    command(0x60);
    write_data(config);

    command(0xAE);
    kbd_present = true;

    command(0xA8);
    command(0x20);
    if ((r = read_data()) >= 0 && !(r & 0x20))
        mouse_present = init_mouse();
    while (inb(PS2_STATUS) & STATUS_OUTPUT)
        inb(PS2_DATA);

    config |= 0x01 | (mouse_present ? 0x02 : 0);
    config &= ~(0x10 | (mouse_present ? 0x20 : 0));
    command(0x60);
    write_data(config);

    kbd_dev = input_register("PS/2 keyboard", INPUT_KIND_KEYBOARD, set_leds, NULL);
    if (mouse_present)
        mouse_dev = input_register(mouse_id == 4 ? "PS/2 mouse (5 buttons, wheel)"
                                   : mouse_id == 3 ? "PS/2 mouse (wheel)" : "PS/2 mouse",
                                   INPUT_KIND_POINTER, NULL, NULL);
    if (irq_install_isa(1, ps2_irq) < 0)
        kbd_present = false;
    if (mouse_present && irq_install_isa(12, ps2_irq) < 0)
        mouse_present = false;
    drain();
}

bool ps2_keyboard_present(void)
{
    return kbd_present;
}

bool ps2_mouse_present(void)
{
    return mouse_present;
}
