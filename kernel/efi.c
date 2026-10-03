#include "kernel.h"
#include "cpu.h"
#include "mem.h"
#include "string.h"

// UEFI runtime services, called in physical mode (the kernel never calls
// SetVirtualAddressMap, and all RAM is identity mapped in the kernel's
// page tables). Used for "restart into firmware setup".

#define EFI_SUCCESS                 0
#define EFI_VAR_NV                  0x1
#define EFI_VAR_BS                  0x2
#define EFI_VAR_RT                  0x4
#define OS_INDICATIONS_BOOT_TO_FW_UI 0x1ULL

typedef uint64_t (__attribute__((ms_abi)) *efi_get_variable_fn)(const uint16_t *name, const void *guid,
                                                                 uint32_t *attrs, uint64_t *size, void *data);
typedef uint64_t (__attribute__((ms_abi)) *efi_set_variable_fn)(const uint16_t *name, const void *guid,
                                                                 uint32_t attrs, uint64_t size, const void *data);
typedef void (__attribute__((ms_abi)) *efi_reset_system_fn)(uint32_t type, uint64_t status, uint64_t size,
                                                             const void *data);

// The global variable GUID, 8BE4DF61-93CA-11D2-AA0D-00E098032B8C.
static const uint8_t global_guid[16] = { 0x61, 0xDF, 0xE4, 0x8B, 0xCA, 0x93, 0xD2, 0x11,
                                         0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C };
static uint64_t runtime;            // EFI_RUNTIME_SERVICES *

void efi_init(uint64_t system_table)
{
    // EFI_SYSTEM_TABLE: header (24), vendor, revision, three handle and
    // protocol pairs, then RuntimeServices at offset 88.
    if (system_table)
        runtime = *(uint64_t *)(system_table + 88);
}

static void name16(const char *s, uint16_t *out)
{
    while ((*out++ = (uint8_t)*s++))
        ;
}

// Calls go through the kernel's own page tables, with interrupts off.
#define EFI_CALL(expr) ({                                       \
    uint64_t flags_ = irq_save(), cr3_ = read_cr3(), r_;        \
    write_cr3(paging_kernel_space());                           \
    r_ = (expr);                                                \
    write_cr3(cr3_);                                            \
    irq_restore(flags_);                                        \
    r_;                                                         \
})

static uint64_t get_u64(const char *var, uint64_t *value)
{
    uint16_t name[32];
    uint32_t attrs = 0;
    uint64_t size = 8;
    efi_get_variable_fn get = (efi_get_variable_fn)*(uint64_t *)(runtime + 24 + 6 * 8);

    name16(var, name);
    *value = 0;
    return EFI_CALL(get(name, global_guid, &attrs, &size, value));
}

bool efi_firmware_setup_supported(void)
{
    uint64_t v;

    return runtime && get_u64("OsIndicationsSupported", &v) == EFI_SUCCESS && (v & OS_INDICATIONS_BOOT_TO_FW_UI);
}

// Asks the firmware to open its setup screen on the next start.
int efi_request_firmware_setup(void)
{
    uint16_t name[32];
    uint64_t v = 0;
    efi_set_variable_fn set;

    if (!efi_firmware_setup_supported())
        return -1;
    set = (efi_set_variable_fn)*(uint64_t *)(runtime + 24 + 8 * 8);
    get_u64("OsIndications", &v);
    v |= OS_INDICATIONS_BOOT_TO_FW_UI;
    name16("OsIndications", name);
    return EFI_CALL(set(name, global_guid, EFI_VAR_NV | EFI_VAR_BS | EFI_VAR_RT, 8, &v)) == EFI_SUCCESS ? 0 : -1;
}

void efi_reset_cold(void)
{
    efi_reset_system_fn reset;

    if (!runtime)
        return;
    reset = (efi_reset_system_fn)*(uint64_t *)(runtime + 24 + 10 * 8);
    cli();
    write_cr3(paging_kernel_space());
    reset(0, 0, 0, NULL);           // EfiResetCold; does not return
}
