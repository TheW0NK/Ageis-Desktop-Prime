#include "acpi.h"
#include "cpu.h"
#include "mem.h"
#include "string.h"

struct rsdp {
    char signature[8];
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct sdt {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem[6];
    char oem_table[8];
    uint32_t oem_revision;
    uint32_t creator;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_info acpi;

static bool checksum_ok(const void *p, size_t len)
{
    const uint8_t *b = p;
    uint8_t sum = 0;

    for (size_t i = 0; i < len; i++)
        sum += b[i];
    return sum == 0;
}

static const struct sdt *find_table(const struct rsdp *rsdp, const char *sig)
{
    bool xsdt = rsdp->revision >= 2 && rsdp->xsdt;
    const struct sdt *root = (const struct sdt *)(xsdt ? rsdp->xsdt : rsdp->rsdt);
    size_t entry_size = xsdt ? 8 : 4;
    size_t count;

    if (!root || !checksum_ok(root, root->length))
        return NULL;
    count = (root->length - sizeof(*root)) / entry_size;

    for (size_t i = 0; i < count; i++) {
        const uint8_t *p = (const uint8_t *)(root + 1) + i * entry_size;
        uint64_t addr = xsdt ? *(const uint64_t *)p : *(const uint32_t *)p;
        const struct sdt *t = (const struct sdt *)addr;

        if (t && memcmp(t->signature, sig, 4) == 0 && checksum_ok(t, t->length))
            return t;
    }
    return NULL;
}

static void parse_madt(const struct sdt *madt)
{
    const uint8_t *p = (const uint8_t *)madt + 44;
    const uint8_t *end = (const uint8_t *)madt + madt->length;

    acpi.lapic_address = *(const uint32_t *)((const uint8_t *)madt + 36);
    acpi.legacy_pic = *(const uint32_t *)((const uint8_t *)madt + 40) & 1;

    while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
        switch (p[0]) {
        case 0:
            if ((*(const uint32_t *)(p + 4) & 3) && acpi.cpu_count < ACPI_MAX_CPUS)
                acpi.cpu_apic_ids[acpi.cpu_count++] = p[3];
            break;
        case 1:
            if (acpi.ioapic_count < ACPI_MAX_IOAPICS) {
                struct acpi_ioapic *io = &acpi.ioapics[acpi.ioapic_count++];
                io->id = p[2];
                io->address = *(const uint32_t *)(p + 4);
                io->gsi_base = *(const uint32_t *)(p + 8);
            }
            break;
        case 2:
            if (acpi.override_count < ACPI_MAX_OVERRIDES) {
                struct acpi_override *o = &acpi.overrides[acpi.override_count++];
                o->source = p[3];
                o->gsi = *(const uint32_t *)(p + 4);
                o->flags = *(const uint16_t *)(p + 8);
            }
            break;
        case 5:
            acpi.lapic_address = *(const uint64_t *)(p + 4);
            break;
        }
        p += p[1];
    }
}

// Finds \_S5 in the DSDT without an AML interpreter: the package holds the
// SLP_TYP values for PM1a and PM1b.
static void parse_s5(const struct sdt *dsdt)
{
    const uint8_t *p = (const uint8_t *)(dsdt + 1);
    const uint8_t *end = (const uint8_t *)dsdt + dsdt->length;

    for (; p + 4 < end; p++) {
        if (memcmp(p, "_S5_", 4) != 0)
            continue;
        if (!(p[-1] == 0x08 || (p[-2] == 0x08 && p[-1] == '\\')) || p[4] != 0x12)
            continue;

        p += 5;
        p += ((*p & 0xC0) >> 6) + 2;

        uint16_t values[2];
        for (int i = 0; i < 2; i++) {
            if (p >= end)
                return;
            if (*p == 0x0A)
                p++;
            values[i] = (uint16_t)(*p++) << 10;
        }
        acpi.s5_type_a = values[0];
        acpi.s5_type_b = values[1];
        acpi.s5_valid = true;
        return;
    }
}

static void parse_fadt(const struct sdt *fadt)
{
    const uint8_t *f = (const uint8_t *)fadt;
    uint32_t flags = fadt->length >= 116 ? *(const uint32_t *)(f + 112) : 0;
    uint64_t dsdt = *(const uint32_t *)(f + 40);

    acpi.sci_irq = *(const uint16_t *)(f + 46);
    acpi.smi_cmd = *(const uint32_t *)(f + 48);
    acpi.acpi_enable = f[52];
    acpi.pm1a_control = *(const uint32_t *)(f + 64);
    acpi.pm1b_control = *(const uint32_t *)(f + 68);
    acpi.pm_timer_port = *(const uint32_t *)(f + 76);
    acpi.pm_timer_32bit = flags & (1 << 8);

    if (fadt->length >= 129 && (flags & (1 << 10))) {
        memcpy(&acpi.reset_reg, f + 116, sizeof(acpi.reset_reg));
        acpi.reset_value = f[128];
        acpi.reset_supported = true;
    }
    if (fadt->length >= 148 && *(const uint64_t *)(f + 140))
        dsdt = *(const uint64_t *)(f + 140);
    if (fadt->length >= 220) {
        const struct acpi_gas *x = (const struct acpi_gas *)(f + 208);
        if (x->space == 1 && x->address)
            acpi.pm_timer_port = x->address;
    }

    if (dsdt && checksum_ok((const void *)dsdt, ((const struct sdt *)dsdt)->length)) {
        uint32_t len = ((const struct sdt *)dsdt)->length;

        parse_s5((const struct sdt *)dsdt);
        if ((acpi.dsdt = kmalloc(len))) {
            memcpy(acpi.dsdt, (const void *)dsdt, len);
            acpi.dsdt_length = len;
        }
    }
}

static void parse_mcfg(const struct sdt *mcfg)
{
    if (mcfg->length >= 44 + 16)
        acpi.pcie_ecam = *(const uint64_t *)((const uint8_t *)mcfg + 44);
}

void acpi_init(uint64_t rsdp_addr)
{
    const struct rsdp *rsdp = (const struct rsdp *)rsdp_addr;
    const struct sdt *t;

    if (!rsdp || memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20))
        return;

    acpi.revision = rsdp->revision;
    memcpy(acpi.oem, rsdp->oem, 6);
    acpi.present = true;

    if ((t = find_table(rsdp, "APIC")))
        parse_madt(t);
    if ((t = find_table(rsdp, "FACP")))
        parse_fadt(t);
    if ((t = find_table(rsdp, "MCFG")))
        parse_mcfg(t);
}

void acpi_pm_timer_wait_us(uint64_t us)
{
    uint64_t mask = acpi.pm_timer_32bit ? 0xFFFFFFFFULL : 0xFFFFFFULL;
    uint64_t target = us * 3579545 / 1000000;
    uint64_t start = inl(acpi.pm_timer_port) & mask;
    uint64_t elapsed = 0;

    while (elapsed < target)
        elapsed = ((inl(acpi.pm_timer_port) & mask) - start) & mask;
}

static void write_gas(const struct acpi_gas *gas, uint8_t value)
{
    if (gas->space == 1)
        outb(gas->address, value);
    else if (gas->space == 0)
        *(volatile uint8_t *)gas->address = value;
}

void acpi_reboot(void)
{
    cli();
    if (acpi.reset_supported)
        write_gas(&acpi.reset_reg, acpi.reset_value);

    outb(0xCF9, 0x06);
    for (int i = 0; i < 10000 && (inb(0x64) & 2); i++)
        ;
    outb(0x64, 0xFE);

    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(null_idt));
    halt_forever();
}

void acpi_shutdown(void)
{
    cli();
    if (acpi.s5_valid && acpi.pm1a_control) {
        if (acpi.smi_cmd && acpi.acpi_enable && !(inw(acpi.pm1a_control) & 1)) {
            outb(acpi.smi_cmd, acpi.acpi_enable);
            for (int i = 0; i < 1000000 && !(inw(acpi.pm1a_control) & 1); i++)
                ;
        }
        outw(acpi.pm1a_control, acpi.s5_type_a | (1 << 13));
        if (acpi.pm1b_control)
            outw(acpi.pm1b_control, acpi.s5_type_b | (1 << 13));
    }

    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    halt_forever();
}
