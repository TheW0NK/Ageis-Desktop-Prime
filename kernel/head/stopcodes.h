#ifndef AEGIS_STOPCODES_H
#define AEGIS_STOPCODES_H

#include <stdint.h>

// Stop codes for the crash screen: a name for each kind of fatal error and
// a line of text to go with it. Codes below 0x20 are CPU exception vectors;
// the rest are kernel errors (panic()). Edit freely: only the table is used.

struct stop_code {
    uint32_t code;
    const char *name;
    const char *quip;
};

#define STOP_KERNEL_PANIC       0x100
#define STOP_OUT_OF_MEMORY      0x101
#define STOP_BAD_BOOT_INFO      0x102
#define STOP_NO_ROOT            0x103

static const struct stop_code stop_codes[] = {
    { 0x00, "DIVIDE_BY_ZERO", "Somebody tried to split the bill zero ways." },
    { 0x01, "DEBUG_TRAP", "A breakpoint walked into a bar." },
    { 0x02, "NMI_HARDWARE_FAILURE", "The hardware raised its hand and would not put it down." },
    { 0x03, "BREAKPOINT", "Paused for dramatic effect. Permanently." },
    { 0x04, "ARITHMETIC_OVERFLOW", "Too much of a good thing." },
    { 0x05, "BOUND_RANGE_EXCEEDED", "Out of bounds. The referee has spoken." },
    { 0x06, "INVALID_OPCODE", "The processor read that instruction twice and still did not get it." },
    { 0x07, "FPU_NOT_AVAILABLE", "The maths department is out to lunch." },
    { 0x08, "DOUBLE_FAULT", "One fault was not enough." },
    { 0x0A, "INVALID_TSS", "The task state segment is in a state." },
    { 0x0B, "SEGMENT_NOT_PRESENT", "That segment has left the building." },
    { 0x0C, "STACK_SEGMENT_FAULT", "The stack fell over. Nobody is surprised." },
    { 0x0D, "GENERAL_PROTECTION_FAULT", "The kernel touched something it was told not to touch." },
    { 0x0E, "PAGE_FAULT_IN_KERNEL", "A page went missing. We looked behind the sofa." },
    { 0x10, "X87_FLOATING_POINT_ERROR", "Floating point, sinking fast." },
    { 0x11, "ALIGNMENT_CHECK", "Something was not quite lined up." },
    { 0x12, "MACHINE_CHECK_EXCEPTION", "The machine checked. It did not like what it saw." },
    { 0x13, "SIMD_FLOATING_POINT_ERROR", "Several numbers went wrong at once, very efficiently." },
    { 0x14, "VIRTUALIZATION_EXCEPTION", "Reality is not what it used to be." },
    { 0x15, "CONTROL_PROTECTION_FAULT", "A jump went somewhere it should never have gone." },
    { 0xFF, "UNEXPECTED_CPU_EXCEPTION", "The processor objected, in a way nobody planned for." },
    { STOP_KERNEL_PANIC, "KERNEL_PANIC", "The kernel has had enough for today." },
    { STOP_OUT_OF_MEMORY, "KERNEL_OUT_OF_MEMORY", "Every last byte is spoken for." },
    { STOP_BAD_BOOT_INFO, "BAD_BOOT_INFO", "The bootloader and the kernel are not on speaking terms." },
    { STOP_NO_ROOT, "NO_ROOT_FILESYSTEM", "There is nowhere to stand." },
};

static inline const struct stop_code *stop_code_lookup(uint32_t code)
{
    for (unsigned i = 0; i < sizeof(stop_codes) / sizeof(stop_codes[0]); i++)
        if (stop_codes[i].code == code)
            return &stop_codes[i];
    return 0;
}

static inline const struct stop_code *stop_code_find(uint32_t code)
{
    const struct stop_code *s = stop_code_lookup(code);

    return s ? s : stop_code_lookup(code < 0x20 ? 0xFF : STOP_KERNEL_PANIC);
}

#endif
