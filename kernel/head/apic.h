#ifndef AEGIS_APIC_H
#define AEGIS_APIC_H

#include "kernel.h"
#include "cpu.h"

#define TIMER_HZ    100

void apic_init(void);
void apic_eoi(void);
uint32_t apic_id(void);
bool ioapic_route_isa(uint8_t irq, uint8_t vector);
void timer_init(void);
uint64_t timer_ticks(void);
uint64_t timer_uptime_ms(void);
int irq_install_isa(uint8_t irq, irq_handler_t handler);
void lapic_init_ap(void);
void lapic_send_init(uint32_t apic);
void lapic_send_sipi(uint32_t apic, uint8_t page);
void lapic_send_ipi_all_others(uint8_t vector);

#endif
