#ifndef AEGIS_INPUT_H
#define AEGIS_INPUT_H

#include "kernel.h"
#include "abi/input.h"

struct file;

#define INPUT_KIND_KEYBOARD 0x1
#define INPUT_KIND_POINTER  0x2
#define INPUT_KIND_TABLET   0x4

// Drivers register each device once and then report through it. All report
// functions may be called from interrupt context.
int input_register(const char *name, uint32_t kind, void (*set_leds)(void *ctx, uint32_t mods), void *ctx);
void input_unregister(int dev);
void input_key(int dev, uint16_t code, bool pressed);
void input_rel(int dev, uint16_t code, int32_t delta);
void input_abs(int dev, uint16_t code, int32_t value);
void input_sync(int dev);
uint32_t input_modifiers(void);
bool input_key_down(uint16_t code);

// Starts the autorepeat thread. Drivers report presses and releases only;
// repeats are generated here so every keyboard repeats at the same rate.
void input_init(void);

struct file *input_open(void);
int input_device_count(void);
const char *input_device_name(int dev, uint32_t *kind);

#endif
