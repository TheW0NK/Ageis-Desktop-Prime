bits 64

extern kmain

section .text

global kernel_entry
kernel_entry:
    ; Leave the bootloader's stack, which lives in reclaimable memory.
    lea     rsp, [rel stack_top]
    xor     ebp, ebp
    call    kmain
.hang:
    cli
    hlt
    jmp     .hang

section .bss
align 16
    resb    64 * 1024
stack_top:
