ENTRY(kernel_entry)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_VMA;
    __kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4096) {
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA) ALIGN(4096) {
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_VMA) ALIGN(4096) {
        *(.data .data.*)
    }

    .bss : AT(ADDR(.bss) - KERNEL_VMA) ALIGN(4096) {
        *(.bss .bss.* COMMON)
    }

    __kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
