bits 64

extern isr_dispatch
extern syscall_dispatch

; struct cpu offsets, see percpu.h
CPU_KERNEL_RSP  equ 8
CPU_USER_RSP    equ 16
CPU_SWITCH_DONE equ 24

section .text

%macro ISR_NOERR 1
isr_%1:
    push    0
    push    %1
    jmp     isr_common
%endmacro

%macro ISR_ERR 1
isr_%1:
    push    %1
    jmp     isr_common
%endmacro

%assign v 0
%rep 256
%if v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21 || v == 29 || v == 30
    ISR_ERR v
%else
    ISR_NOERR v
%endif
%assign v v + 1
%endrep

; isr_dispatch returns the frame to resume, which may belong to another thread.
isr_common:
    test    qword [rsp + 24], 3
    jz      .from_kernel
    swapgs
.from_kernel:
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    cld
    mov     rdi, rsp
    call    isr_dispatch
    mov     rsp, rax

    ; The previous thread's stack is no longer in use; let other CPUs run it.
    mov     rax, [gs:CPU_SWITCH_DONE]
    test    rax, rax
    jz      .no_switch
    mov     byte [rax], 0
    mov     qword [gs:CPU_SWITCH_DONE], 0
.no_switch:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax
    add     rsp, 16
    test    qword [rsp + 8], 3
    jz      .to_kernel
    swapgs
.to_kernel:
    iretq

global syscall_entry
syscall_entry:
    swapgs
    mov     [gs:CPU_USER_RSP], rsp
    mov     rsp, [gs:CPU_KERNEL_RSP]
    push    qword [gs:CPU_USER_RSP]
    push    rcx
    push    r11
    push    rax
    push    rbx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r12
    push    r13
    push    r14
    push    r15

    cld
    mov     rdi, rsp
    call    syscall_dispatch

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rbx
    pop     rax
    pop     r11
    pop     rcx
    pop     rsp
    swapgs
    o64 sysret

section .rodata

global isr_table
isr_table:
%assign v 0
%rep 256
    dq      isr_ %+ v
%assign v v + 1
%endrep
