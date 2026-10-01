#ifndef AEGIS_ACPI_H
#define AEGIS_ACPI_H

#include "kernel.h"

#define ACPI_MAX_CPUS       64
#define ACPI_MAX_IOAPICS    8
#define ACPI_MAX_OVERRIDES  16

struct acpi_ioapic {
    uint8_t id;
    uint64_t address;
    uint32_t gsi_base;
};

struct acpi_override {
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
};

struct acpi_gas {
    uint8_t space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

struct acpi_info {
    bool present;
    uint8_t revision;
    char oem[7];

    uint64_t lapic_address;
    bool legacy_pic;
    uint32_t cpu_count;
    uint8_t cpu_apic_ids[ACPI_MAX_CPUS];
    uint32_t ioapic_count;
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    uint32_t override_count;
    struct acpi_override overrides[ACPI_MAX_OVERRIDES];

    uint16_t sci_irq;
    uint32_t pm_timer_port;
    bool pm_timer_32bit;
    bool reset_supported;
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    uint16_t smi_cmd;
    uint8_t acpi_enable;
    uint32_t pm1a_control;
    uint32_t pm1b_control;
    bool s5_valid;
    uint16_t s5_type_a;
    uint16_t s5_type_b;
    uint64_t pcie_ecam;
    void *dsdt;                     // copy that survives mem_reclaim_acpi()
    uint32_t dsdt_length;
};

extern struct acpi_info acpi;

void acpi_init(uint64_t rsdp);
NORETURN void acpi_reboot(void);
NORETURN void acpi_shutdown(void);
void acpi_pm_timer_wait_us(uint64_t us);

#endif
