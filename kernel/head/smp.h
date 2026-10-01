#ifndef AEGIS_SMP_H
#define AEGIS_SMP_H

#include "kernel.h"

#define VECTOR_TLB      0xFD
#define VECTOR_HALT     0xFE

void smp_init(void);
bool smp_active(void);
void tlb_shootdown(void);
void smp_halt_others(void);

#endif
