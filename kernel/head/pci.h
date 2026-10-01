#ifndef AEGIS_PCI_H
#define AEGIS_PCI_H

#include "kernel.h"

#define PCI_MAX_DEVICES 128

struct pci_device {
    uint8_t bus, slot, function;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, revision;
    uint8_t irq_line;
    uint32_t bar[6];
};

void pci_init(void);
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
size_t pci_device_count(void);
const struct pci_device *pci_device_at(size_t index);
const char *pci_class_name(uint8_t class_code, uint8_t subclass);
const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, size_t nth);
const struct pci_device *pci_find_id(uint16_t vendor, uint16_t device, size_t nth);
void pci_enable(const struct pci_device *d);
uint64_t pci_bar(const struct pci_device *d, int index, bool *is_io);

#endif
