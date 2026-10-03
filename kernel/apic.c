#include "stopcodes.h"
#include "apic.h"
#include "acpi.h"
#include "mem.h"
#include "percpu.h"
#include "sched.h"

#define LAPIC_ID            0x020
#define LAPIC_TPR           0x080
#define LAPIC_EOI           0x0B0
#define LAPIC_SVR           0x0F0
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_TIMER_INIT    0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIV     0x3E0

#define LVT_MASKED          (1 << 16)
#define LVT_PERIODIC        (1 << 17)

#define IOAPIC_VERSION      0x01
#define IOAPIC_REDIRECT     0x10

#define LAPIC_ICR_LOW       0x300
#define LAPIC_ICR_HIGH      0x310

static volatile uint32_t *lapic;
static volatile uint64_t ticks;
static uint32_t timer_count;

static uint32_t lapic_read(uint32_t reg)
{
    return lapic[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t value)
{
    lapic[reg / 4] = value;
}

static uint32_t ioapic_read(const struct acpi_ioapic *io, uint32_t reg)
{
    volatile uint32_t *base = (volatile uint32_t *)io->address;

    base[0] = reg;
    return base[4];
}

static void ioapic_write(const struct acpi_ioapic *io, uint32_t reg, uint32_t value)
{
    volatile uint32_t *base = (volatile uint32_t *)io->address;

    base[0] = reg;
    base[4] = value;
}

static uint32_t ioapic_entries(const struct acpi_ioapic *io)
{
    return ((ioapic_read(io, IOAPIC_VERSION) >> 16) & 0xFF) + 1;
}

static void pic_disable(void)
{
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0xF8);
    outb(0xA1, 0xF8);
    outb(0x21, 4);
    outb(0xA1, 2);
    outb(0x21, 1);
    outb(0xA1, 1);
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

// Newer firmware can leave the local APIC in x2APIC mode, where the
// memory-mapped registers used here do nothing (no timer, no interrupts
// after the first). Switch it back to xAPIC mode: x2APIC cannot be turned
// off directly, so the APIC is disabled, then enabled without it.
static void lapic_use_xapic(void)
{
    uint64_t b = rdmsr(MSR_APIC_BASE);

    if (b & (1 << 10)) {
        wrmsr(MSR_APIC_BASE, b & ~((1ULL << 10) | (1ULL << 11)));
        b &= ~(1ULL << 10);
    }
    wrmsr(MSR_APIC_BASE, b | (1 << 11));
}

void apic_init(void)
{
    uint64_t base;

    pic_disable();

    base = acpi.lapic_address ? acpi.lapic_address : (rdmsr(MSR_APIC_BASE) & ~0xFFFULL);
    if (!paging_map_mmio(base, PAGE_SIZE))
        panic_code(STOP_APIC_FAILED, "Cannot map the local APIC at 0x%lx", base);
    lapic = (volatile uint32_t *)base;
    lapic_use_xapic();

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, 0x100 | VECTOR_SPURIOUS);

    for (uint32_t i = 0; i < acpi.ioapic_count; i++) {
        const struct acpi_ioapic *io = &acpi.ioapics[i];

        if (!paging_map_mmio(io->address, PAGE_SIZE))
            panic_code(STOP_APIC_FAILED, "Cannot map IO APIC %u at 0x%lx", io->id, io->address);
        for (uint32_t n = 0; n < ioapic_entries(io); n++)
            ioapic_write(io, IOAPIC_REDIRECT + n * 2, LVT_MASKED);
    }
}

void apic_eoi(void)
{
    if (lapic)
        lapic_write(LAPIC_EOI, 0);
}

uint32_t apic_id(void)
{
    return lapic ? lapic_read(LAPIC_ID) >> 24 : 0;
}

bool ioapic_route_isa(uint8_t irq, uint8_t vector)
{
    uint32_t gsi = irq;
    uint16_t flags = 0;
    uint32_t low;

    for (uint32_t i = 0; i < acpi.override_count; i++) {
        if (acpi.overrides[i].source == irq) {
            gsi = acpi.overrides[i].gsi;
            flags = acpi.overrides[i].flags;
        }
    }

    low = vector;
    if ((flags & 3) == 3)
        low |= 1 << 13;
    if (((flags >> 2) & 3) == 3)
        low |= 1 << 15;

    for (uint32_t i = 0; i < acpi.ioapic_count; i++) {
        const struct acpi_ioapic *io = &acpi.ioapics[i];

        if (gsi >= io->gsi_base && gsi < io->gsi_base + ioapic_entries(io)) {
            uint32_t n = gsi - io->gsi_base;

            ioapic_write(io, IOAPIC_REDIRECT + n * 2 + 1, apic_id() << 24);
            ioapic_write(io, IOAPIC_REDIRECT + n * 2, low);
            return true;
        }
    }
    return false;
}

int irq_install_isa(uint8_t irq, irq_handler_t handler)
{
    uint8_t vector = VECTOR_ISA_BASE + irq;

    irq_register(vector, handler);
    return ioapic_route_isa(irq, vector) ? vector : -1;
}

static void wait_10ms(void)
{
    if (acpi.pm_timer_port) {
        acpi_pm_timer_wait_us(10000);
        return;
    }

    uint16_t count = 1193182 / 100;
    outb(0x61, (inb(0x61) & 0xFD) | 1);
    outb(0x43, 0xB0);
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    uint8_t v = inb(0x61) & 0xFE;
    outb(0x61, v);
    outb(0x61, v | 1);
    while (!(inb(0x61) & 0x20))
        ;
}

static void timer_irq(struct interrupt_frame *frame)
{
    (void)frame;
    if (this_cpu()->id == 0)
        ticks++;
}

void timer_init(void)
{
    uint32_t per_10ms;

    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
    wait_10ms();
    per_10ms = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CURRENT);
    lapic_write(LAPIC_TIMER_INIT, 0);

    if (per_10ms == 0)
        panic_code(STOP_TIMER_FAILED, "Local APIC timer calibration failed");

    irq_register(VECTOR_TIMER, timer_irq);
    timer_count = per_10ms * 100 / TIMER_HZ;
    lapic_write(LAPIC_LVT_TIMER, VECTOR_TIMER | LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, timer_count);
}

uint64_t timer_ticks(void)
{
    return ticks;
}

uint64_t timer_uptime_ms(void)
{
    return ticks * 1000 / TIMER_HZ;
}

void lapic_init_ap(void)
{
    lapic_use_xapic();
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, 0x100 | VECTOR_SPURIOUS);
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, VECTOR_TIMER | LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, timer_count);
}

static void icr_send(uint32_t high, uint32_t low)
{
    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12))
        __asm__ volatile ("pause");
    lapic_write(LAPIC_ICR_HIGH, high);
    lapic_write(LAPIC_ICR_LOW, low);
    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12))
        __asm__ volatile ("pause");
}

void lapic_send_init(uint32_t apic)
{
    icr_send(apic << 24, 0x4500);
}

void lapic_send_sipi(uint32_t apic, uint8_t page)
{
    icr_send(apic << 24, 0x4600 | page);
}

void lapic_send_ipi_all_others(uint8_t vector)
{
    icr_send(0, (3 << 18) | (1 << 14) | vector);
}
