#include "pci.h"
#include "cpu.h"
#include "apic.h"
#include "mem.h"

#define PCI_ADDRESS     0xCF8
#define PCI_DATA        0xCFC

static struct pci_device devices[PCI_MAX_DEVICES];
static size_t count;

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    outl(PCI_ADDRESS, 0x80000000 | (bus << 16) | (slot << 11) | (function << 8) | (offset & 0xFC));
    return inl(PCI_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value)
{
    outl(PCI_ADDRESS, 0x80000000 | (bus << 16) | (slot << 11) | (function << 8) | (offset & 0xFC));
    outl(PCI_DATA, value);
}

static void add(uint8_t bus, uint8_t slot, uint8_t function, uint32_t id)
{
    struct pci_device *d;
    uint32_t class_reg = pci_read32(bus, slot, function, 0x08);

    if (count == PCI_MAX_DEVICES)
        return;
    d = &devices[count++];
    d->bus = bus;
    d->slot = slot;
    d->function = function;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    d->class_code = class_reg >> 24;
    d->subclass = class_reg >> 16;
    d->prog_if = class_reg >> 8;
    d->revision = class_reg;
    d->irq_line = pci_read32(bus, slot, function, 0x3C);

    if (((pci_read32(bus, slot, function, 0x0C) >> 16) & 0x7F) == 0) {
        for (int i = 0; i < 6; i++)
            d->bar[i] = pci_read32(bus, slot, function, 0x10 + i * 4);
    }
}

void pci_init(void)
{
    for (unsigned bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32(bus, slot, 0, 0);
            uint8_t functions;

            if ((id & 0xFFFF) == 0xFFFF)
                continue;
            functions = (pci_read32(bus, slot, 0, 0x0C) >> 16) & 0x80 ? 8 : 1;

            for (uint8_t fn = 0; fn < functions; fn++) {
                id = pci_read32(bus, slot, fn, 0);
                if ((id & 0xFFFF) != 0xFFFF)
                    add(bus, slot, fn, id);
            }
        }
    }
}

size_t pci_device_count(void)
{
    return count;
}

const struct pci_device *pci_device_at(size_t index)
{
    return index < count ? &devices[index] : NULL;
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass)
{
    switch (class_code) {
    case 0x01:
        switch (subclass) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default: return "Storage controller";
        }
    case 0x02: return "Network controller";
    case 0x03: return "Display controller";
    case 0x04: return "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        default: return "Bridge";
        }
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input controller";
    case 0x0C:
        return subclass == 0x03 ? "USB controller" : "Serial bus controller";
    case 0x0D: return "Wireless controller";
    default: return "Unknown device";
    }
}

const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, size_t nth)
{
    for (size_t i = 0; i < count; i++) {
        const struct pci_device *d = &devices[i];

        if (d->class_code == class_code && d->subclass == subclass && d->prog_if == prog_if && nth-- == 0)
            return d;
    }
    return NULL;
}

const struct pci_device *pci_find_id(uint16_t vendor, uint16_t device, size_t nth)
{
    for (size_t i = 0; i < count; i++) {
        if (devices[i].vendor == vendor && devices[i].device == device && nth-- == 0)
            return &devices[i];
    }
    return NULL;
}

void pci_enable(const struct pci_device *d)
{
    uint32_t cmd = pci_read32(d->bus, d->slot, d->function, 0x04);

    pci_write32(d->bus, d->slot, d->function, 0x04, (cmd & 0xFFFF) | 0x7);
}

uint64_t pci_bar(const struct pci_device *d, int index, bool *is_io)
{
    uint32_t bar = d->bar[index];

    if (bar & 1) {
        if (is_io)
            *is_io = true;
        return bar & ~3U;
    }
    if (is_io)
        *is_io = false;
    if (((bar >> 1) & 3) == 2 && index < 5)
        return ((uint64_t)d->bar[index + 1] << 32) | (bar & ~0xFU);
    return bar & ~0xFU;
}

// Returns the config-space offset of a capability, or 0.
uint8_t pci_find_capability(const struct pci_device *d, uint8_t id)
{
    uint8_t off;

    if (!(pci_read32(d->bus, d->slot, d->function, 0x04) & (1U << 20)))
        return 0;
    off = pci_read32(d->bus, d->slot, d->function, 0x34) & 0xFC;
    for (int guard = 0; off && guard < 48; guard++) {
        uint32_t cap = pci_read32(d->bus, d->slot, d->function, off);

        if ((cap & 0xFF) == id)
            return off;
        off = (cap >> 8) & 0xFC;
    }
    return 0;
}

// Interrupt vectors for message-signalled interrupts, below the system ones.
#define MSI_VECTOR_FIRST    0x50
#define MSI_VECTOR_LAST     0xEF

static uint8_t next_vector = MSI_VECTOR_FIRST;

int pci_alloc_vector(irq_handler_t handler)
{
    uint8_t v;

    if (next_vector == VECTOR_YIELD)
        next_vector++;
    if (next_vector > MSI_VECTOR_LAST)
        return -1;
    v = next_vector++;
    irq_register(v, handler);
    return v;
}

static uint32_t msi_address(void)
{
    return 0xFEE00000U | (apic_id() << 12);
}

// Routes the device's interrupt to handler through MSI-X (entry 0 for every
// vector table entry the device uses) or MSI. Returns the vector, or -1.
int pci_enable_msi(const struct pci_device *d, irq_handler_t handler)
{
    uint8_t cap = pci_find_capability(d, 0x05);
    int vector;

    if (!cap)
        return -1;
    if ((vector = pci_alloc_vector(handler)) < 0)
        return -1;

    uint32_t ctrl = pci_read32(d->bus, d->slot, d->function, cap);
    bool is64 = ctrl & (1U << 23);

    pci_write32(d->bus, d->slot, d->function, cap + 4, msi_address());
    if (is64) {
        pci_write32(d->bus, d->slot, d->function, cap + 8, 0);
        pci_write32(d->bus, d->slot, d->function, cap + 12, vector);
    } else {
        pci_write32(d->bus, d->slot, d->function, cap + 8, vector);
    }
    // One message, enabled; legacy INTx off.
    ctrl &= ~(0x7U << 20);
    ctrl |= 1U << 16;
    pci_write32(d->bus, d->slot, d->function, cap, ctrl);
    pci_write32(d->bus, d->slot, d->function, 0x04,
                (pci_read32(d->bus, d->slot, d->function, 0x04) & 0xFFFF) | (1U << 10));
    return vector;
}

// MSI-X with `count` table entries, all delivering the same handler on
// consecutive vectors. Returns the first vector, or -1.
int pci_enable_msix(const struct pci_device *d, int count, irq_handler_t handler)
{
    uint8_t cap = pci_find_capability(d, 0x11);
    uint32_t ctrl, table;
    volatile uint32_t *entries;
    uint64_t bar;
    int first = -1;

    if (!cap)
        return -1;
    ctrl = pci_read32(d->bus, d->slot, d->function, cap);
    if ((int)((ctrl >> 16) & 0x7FF) + 1 < count)
        return -1;
    table = pci_read32(d->bus, d->slot, d->function, cap + 4);
    bar = pci_bar(d, table & 7, NULL);
    if (!bar || !paging_map_mmio(bar + (table & ~7U), count * 16))
        return -1;
    entries = (volatile uint32_t *)(bar + (table & ~7U));
    for (int i = 0; i < count; i++) {
        int v = pci_alloc_vector(handler);

        if (v < 0)
            return -1;
        if (first < 0)
            first = v;
        entries[i * 4 + 0] = msi_address();
        entries[i * 4 + 1] = 0;
        entries[i * 4 + 2] = v;
        entries[i * 4 + 3] = 0;         // unmasked
    }
    pci_write32(d->bus, d->slot, d->function, cap, (ctrl & ~(1U << 30)) | (1U << 31));
    pci_write32(d->bus, d->slot, d->function, 0x04,
                (pci_read32(d->bus, d->slot, d->function, 0x04) & 0xFFFF) | (1U << 10));
    return first;
}
