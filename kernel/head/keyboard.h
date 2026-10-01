#ifndef AEGIS_KEYBOARD_H
#define AEGIS_KEYBOARD_H

#include "kernel.h"

void keyboard_init(void);
bool keyboard_present(void);
void xhci_init(void);
int xhci_keyboard_count(void);

#endif
