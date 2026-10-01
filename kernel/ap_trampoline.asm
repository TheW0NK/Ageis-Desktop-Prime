; Startup code for application processors. Copied below 1 MiB at runtime and
; entered in real mode via SIPI; switches to long mode on the kernel's page
; tables and calls ap_entry(cpu). The data slots at the end are filled in by
; the bootstrap processor before each start.

TRAMPOLINE_BASE equ 0x8000
%define R(x) (TRAMPOLINE_BASE + (x) - ap_trampoline)

section .rodata

global ap_trampoline
global ap_trampoline_end
global ap_slot_cr3
global ap_slot_stack
global ap_slot_cpu
global ap_slot_entry

bits 16
ap_trampoline:
    cli
    cld
    xor     ax, ax
    mov     ds, ax
    lgdt    [R(gdt_ptr)]
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax
    jmp     dword 0x08:R(protected)

bits 32
protected:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     eax, cr4
    or      eax, 1 << 5
    mov     cr4, eax
    mov     eax, [R(ap_slot_cr3)]
    mov     cr3, eax
    mov     ecx, 0xC0000080
    rdmsr
    or      eax, 1 << 8
    wrmsr
    mov     eax, cr0
    or      eax, 0x80000001
    mov     cr0, eax
    jmp     0x18:R(long_mode)

bits 64
long_mode:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     rsp, [R(ap_slot_stack)]
    mov     rdi, [R(ap_slot_cpu)]
    mov     rax, [R(ap_slot_entry)]
    call    rax
.hang:
    cli
    hlt
    jmp     .hang

align 8
gdt:
    dq      0
    dq      0x00CF9A000000FFFF
    dq      0x00CF92000000FFFF
    dq      0x00AF9A000000FFFF
gdt_ptr:
    dw      gdt_ptr - gdt - 1
    dd      R(gdt)

align 8
ap_slot_cr3:    dq 0
ap_slot_stack:  dq 0
ap_slot_cpu:    dq 0
ap_slot_entry:  dq 0
ap_trampoline_end:
