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
#define STOP_CRITICAL_PROCESS   0x104
#define STOP_HEAP_CORRUPTION    0x105
#define STOP_BAD_PAGE_FREE      0x106
#define STOP_NO_ACPI            0x107
#define STOP_APIC_FAILED        0x108
#define STOP_TIMER_FAILED       0x109
#define STOP_THREAD_START       0x10A
#define STOP_MEMORY_MAP         0x10B
#define STOP_MANUAL_CRASH       0x10C
#define STOP_DRIVER_FAULT       0x10D
#define STOP_FILESYSTEM_CORRUPT 0x10E
#define STOP_STACK_OVERFLOW     0x10F

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
    // Movie quotes and computer jokes.
    { STOP_CRITICAL_PROCESS, "CRITICAL_PROCESS_DIED", "\"I'll be back.\" - The Terminator" },
    { STOP_HEAP_CORRUPTION, "KERNEL_HEAP_CORRUPTION", "It's not a bug, it's an undocumented feature." },
    { STOP_BAD_PAGE_FREE, "BAD_PAGE_FREE", "\"Houston, we have a problem.\" - Apollo 13" },
    { STOP_NO_ACPI, "ACPI_TABLES_MISSING", "\"Toto, I've a feeling we're not in Kansas anymore.\" - The Wizard of Oz" },
    { STOP_APIC_FAILED, "INTERRUPT_CONTROLLER_FAILURE", "\"Great Scott!\" - Back to the Future" },
    { STOP_TIMER_FAILED, "CLOCK_CALIBRATION_FAILED", "\"Roads? Where we're going, we don't need roads.\" - Back to the Future" },
    { STOP_THREAD_START, "KERNEL_THREAD_START_FAILED", "\"I'm sorry, Dave. I'm afraid I can't do that.\" - 2001: A Space Odyssey" },
    { STOP_MEMORY_MAP, "MEMORY_MAP_TOO_LARGE", "\"You're gonna need a bigger boat.\" - Jaws" },
    { STOP_MANUAL_CRASH, "MANUALLY_INITIATED_CRASH", "\"Have you tried turning it off and on again?\" - The IT Crowd" },
    { STOP_DRIVER_FAULT, "DRIVER_FAULT", "There are 10 kinds of people: those who understand binary and those who don't." },
    { STOP_FILESYSTEM_CORRUPT, "FILESYSTEM_CORRUPTION", "\"There is no spoon.\" - The Matrix" },
    { STOP_STACK_OVERFLOW, "KERNEL_STACK_OVERFLOW", "To understand recursion, you must first understand recursion." },
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
