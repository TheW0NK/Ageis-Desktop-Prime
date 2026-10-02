#ifndef AEGIS_ABI_INPUT_H
#define AEGIS_ABI_INPUT_H

#include <stdint.h>

// Records read from /dev/input. Every driver reports through the same codes,
// so a program never needs to know whether a key came from PS/2 or USB.
struct input_event {
    uint64_t time_ms;           // milliseconds since boot
    uint16_t device;            // index of the reporting device
    uint16_t type;              // EV_*
    uint16_t code;              // KEY_*, BTN_*, REL_* or ABS_*
    uint16_t reserved;
    int32_t value;
    uint32_t modifiers;         // MOD_* state after this event
};

#define EV_SYN          0       // end of a group of events from one report
// EV_SYN codes sent only to grabbing readers when the user switches between
// the desktop (Ctrl+Alt+F1) and the text console (Ctrl+Alt+F2).
#define SYN_REPORT      0
#define SYN_VT_LEAVE    1       // stop drawing: the text console owns the screen
#define SYN_VT_ENTER    2       // the screen is back: redraw everything
#define EV_KEY          1       // value: 0 release, 1 press, 2 autorepeat
#define EV_REL          2       // value: signed delta
#define EV_ABS          3       // value: 0 .. INPUT_ABS_MAX

#define REL_X           0
#define REL_Y           1
#define REL_WHEEL       2       // positive is away from the user
#define REL_HWHEEL      3       // positive is to the right

#define ABS_X           0
#define ABS_Y           1
#define INPUT_ABS_MAX   32767

#define MOD_LCTRL       0x0001
#define MOD_LSHIFT      0x0002
#define MOD_LALT        0x0004
#define MOD_LMETA       0x0008
#define MOD_RCTRL       0x0010
#define MOD_RSHIFT      0x0020
#define MOD_RALT        0x0040
#define MOD_RMETA       0x0080
#define MOD_CAPSLOCK    0x0100
#define MOD_NUMLOCK     0x0200
#define MOD_SCROLLLOCK  0x0400
#define MOD_CTRL        (MOD_LCTRL | MOD_RCTRL)
#define MOD_SHIFT       (MOD_LSHIFT | MOD_RSHIFT)
#define MOD_ALT         (MOD_LALT | MOD_RALT)
#define MOD_META        (MOD_LMETA | MOD_RMETA)
// The modifiers that are held down (not the lock states).
#define MOD_KEYS        (MOD_CTRL | MOD_SHIFT | MOD_ALT | MOD_META)

// Key codes 0x00-0xE7 are USB HID keyboard usages (page 0x07).
#define KEY_A           0x04    // A..Z are 0x04..0x1D
#define KEY_1           0x1E    // 1..9 are 0x1E..0x26
#define KEY_0           0x27
#define KEY_ENTER       0x28
#define KEY_ESC         0x29
#define KEY_BACKSPACE   0x2A
#define KEY_TAB         0x2B
#define KEY_SPACE       0x2C
#define KEY_MINUS       0x2D
#define KEY_EQUAL       0x2E
#define KEY_LEFTBRACE   0x2F
#define KEY_RIGHTBRACE  0x30
#define KEY_BACKSLASH   0x31
#define KEY_HASHTILDE   0x32    // non-US # and ~
#define KEY_SEMICOLON   0x33
#define KEY_APOSTROPHE  0x34
#define KEY_GRAVE       0x35
#define KEY_COMMA       0x36
#define KEY_DOT         0x37
#define KEY_SLASH       0x38
#define KEY_CAPSLOCK    0x39
#define KEY_F1          0x3A    // F1..F12 are 0x3A..0x45
#define KEY_F12         0x45
#define KEY_SYSRQ       0x46    // Print Screen
#define KEY_SCROLLLOCK  0x47
#define KEY_PAUSE       0x48
#define KEY_INSERT      0x49
#define KEY_HOME        0x4A
#define KEY_PAGEUP      0x4B
#define KEY_DELETE      0x4C
#define KEY_END         0x4D
#define KEY_PAGEDOWN    0x4E
#define KEY_RIGHT       0x4F
#define KEY_LEFT        0x50
#define KEY_DOWN        0x51
#define KEY_UP          0x52
#define KEY_NUMLOCK     0x53
#define KEY_KPSLASH     0x54
#define KEY_KPASTERISK  0x55
#define KEY_KPMINUS     0x56
#define KEY_KPPLUS      0x57
#define KEY_KPENTER     0x58
#define KEY_KP1         0x59    // KP1..KP9 are 0x59..0x61
#define KEY_KP0         0x62
#define KEY_KPDOT       0x63
#define KEY_102ND       0x64    // non-US \ and |
#define KEY_COMPOSE     0x65    // Menu / Application
#define KEY_POWER       0x66
#define KEY_KPEQUAL     0x67
#define KEY_F13         0x68    // F13..F24 are 0x68..0x73
#define KEY_F24         0x73
#define KEY_HELP        0x75
#define KEY_MUTE        0x7F
#define KEY_VOLUMEUP    0x80
#define KEY_VOLUMEDOWN  0x81
#define KEY_KPCOMMA     0x85
#define KEY_RO          0x87
#define KEY_KATAKANAHIRAGANA 0x88
#define KEY_YEN         0x89
#define KEY_HENKAN      0x8A
#define KEY_MUHENKAN    0x8B
#define KEY_HANGEUL     0x90
#define KEY_HANJA       0x91
#define KEY_LEFTCTRL    0xE0
#define KEY_LEFTSHIFT   0xE1
#define KEY_LEFTALT     0xE2
#define KEY_LEFTMETA    0xE3
#define KEY_RIGHTCTRL   0xE4
#define KEY_RIGHTSHIFT  0xE5
#define KEY_RIGHTALT    0xE6
#define KEY_RIGHTMETA   0xE7

// Media, browser and system keys (from the HID consumer and desktop pages).
#define KEY_PLAYPAUSE   0x100
#define KEY_STOPCD      0x101
#define KEY_PREVIOUSSONG 0x102
#define KEY_NEXTSONG    0x103
#define KEY_EJECTCD     0x104
#define KEY_CALC        0x105
#define KEY_MAIL        0x106
#define KEY_WWW         0x107   // browser home
#define KEY_SEARCH      0x108
#define KEY_BACK        0x109
#define KEY_FORWARD     0x10A
#define KEY_REFRESH     0x10B
#define KEY_BOOKMARKS   0x10C
#define KEY_COMPUTER    0x10D   // "My Computer" / file manager
#define KEY_SLEEP       0x10E
#define KEY_WAKEUP      0x10F
#define KEY_BRIGHTNESSUP   0x110
#define KEY_BRIGHTNESSDOWN 0x111
#define KEY_MEDIA       0x112
#define KEY_CAMERA      0x113

// Pointer buttons.
#define BTN_LEFT        0x200
#define BTN_RIGHT       0x201
#define BTN_MIDDLE      0x202
#define BTN_SIDE        0x203   // back
#define BTN_EXTRA       0x204   // forward

#define KEY_CODE_MAX    0x210

// ioctls on /dev/input.
#define IOCTL_INPUT_GRAB        0x100   // arg 1: stop delivering keys to the text console
#define IOCTL_INPUT_DEVICES     0x101   // returns the number of input devices
#define IOCTL_INPUT_VT          0x102   // returns 1 while the text console is in front

#endif
