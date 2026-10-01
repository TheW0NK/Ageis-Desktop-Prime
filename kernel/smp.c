#include "smp.h"
#include "acpi.h"
#include "apic.h"
#include "mem.h"
#include "percpu.h"
#include "sched.h"
#include "spinlock.h"
#include "string.h"
#include "syscall.h"

#define TRAMPOLINE_BASE 0x8000
#define AP_STACK_SIZE   (16 * 1024)

extern const uint8_t ap_trampoline[], ap_trampoline_end[];
extern const uint8_t ap_slot_cr3[], ap_slot_stack[], ap_slot_cpu[], ap_slot_entry[];

static volatile bool active;
static volatile uint32_t tlb_pending;
static spinlock_t tlb_lock = SPINLOCK_INIT;

bool smp_active(void)
{
    return active;
}

static void tlb_irq(struct interrupt_frame *frame)
{
    (void)frame;
    write_cr3(read_cr3());
    __atomic_fetch_sub(&tlb_pending, 1, __ATOMIC_RELEASE);
}

static void halt_irq(struct interrupt_frame *frame)
{
    (void)frame;
    halt_forever();
}

void tlb_shootdown(void)
{
    uint32_t others = 0;

    if (!active)
        return;
    for (uint32_t i = 0; i < cpu_count; i++)
        others += cpus[i].online && &cpus[i] != this_cpu();
    if (!others)
        return;
    // Spin with interrupts enabled so a concurrent shootdown from another CPU
    // can still be serviced.
    spin_lock(&tlb_lock);
    __atomic_store_n(&tlb_pending, others, __ATOMIC_RELEASE);
    lapic_send_ipi_all_others(VECTOR_TLB);
    for (uint64_t spins = 0; __atomic_load_n(&tlb_pending, __ATOMIC_ACQUIRE) && spins < 100000000; spins++)
        __asm__ volatile ("pause");
    spin_unlock(&tlb_lock);
}

void smp_halt_others(void)
{
    if (active)
        lapic_send_ipi_all_others(VECTOR_HALT);
}

static void slot(const uint8_t *sym, uint64_t value)
{
    *(volatile uint64_t *)(TRAMPOLINE_BASE + (sym - ap_trampoline)) = value;
}

static NORETURN void ap_entry(struct cpu *c)
{
    cpu_setup(c);
    lapic_init_ap();
    syscall_init();
    c->current = c->idle;
    c->tss.rsp[0] = 0;
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    for (;;)
        __asm__ volatile ("sti; hlt");
}

static bool start_cpu(struct cpu *c)
{
    uint8_t *stack = kmalloc(AP_STACK_SIZE);

    c->df_stack = kmalloc(16 * 1024);
    if (!stack || !c->df_stack)
        return false;
    sched_init_cpu(c);
    c->online = false;

    slot(ap_slot_cr3, paging_kernel_space());
    slot(ap_slot_stack, ALIGN_DOWN((uint64_t)stack + AP_STACK_SIZE, 16));
    slot(ap_slot_cpu, (uint64_t)c);
    slot(ap_slot_entry, (uint64_t)ap_entry);

    lapic_send_init(c->apic_id);
    acpi_pm_timer_wait_us(10000);
    for (int i = 0; i < 2 && !c->online; i++) {
        lapic_send_sipi(c->apic_id, TRAMPOLINE_BASE >> 12);
        acpi_pm_timer_wait_us(200);
    }
    for (int ms = 0; ms < 200 && !__atomic_load_n(&c->online, __ATOMIC_ACQUIRE); ms++)
        acpi_pm_timer_wait_us(1000);
    return c->online;
}

void smp_init(void)
{
    uint32_t bsp = apic_id();

    if (acpi.cpu_count <= 1 || !acpi.pm_timer_port)
        return;
    if (paging_kernel_space() >= (4ULL << 30) || !mem_reserve_low(TRAMPOLINE_BASE, PAGE_SIZE)) {
        kprintf("SMP: cannot place the AP trampoline; using one CPU\n");
        return;
    }
    memcpy((void *)TRAMPOLINE_BASE, ap_trampoline, ap_trampoline_end - ap_trampoline);
    irq_register(VECTOR_TLB, tlb_irq);
    irq_register(VECTOR_HALT, halt_irq);
    cpus[0].apic_id = bsp;
    active = true;

    for (uint32_t i = 0; i < acpi.cpu_count && cpu_count < CPU_MAX; i++) {
        struct cpu *c = &cpus[cpu_count];

        if (acpi.cpu_apic_ids[i] == bsp)
            continue;
        c->id = cpu_count;
        c->apic_id = acpi.cpu_apic_ids[i];
        if (start_cpu(c))
            cpu_count++;
        else
            kprintf("SMP: CPU with APIC ID %u did not start\n", c->apic_id);
    }
    kprintf("SMP: %u CPU(s) online\n", cpu_count);
}
