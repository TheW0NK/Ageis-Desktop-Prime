#ifndef AEGIS_KEYBOARD_H
#define AEGIS_KEYBOARD_H

#include "kernel.h"

void ps2_init(void);
bool ps2_keyboard_present(void);
bool ps2_mouse_present(void);
void xhci_init(void);
int xhci_device_count(void);

#endif
