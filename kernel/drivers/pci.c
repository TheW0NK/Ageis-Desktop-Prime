#include "pci.h"
#include "cpu.h"

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
