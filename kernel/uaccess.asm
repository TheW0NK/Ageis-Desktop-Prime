bits 64

; Copies between kernel and user memory. A page fault inside these routines
; that cannot be resolved resumes at uaccess_fixup, which returns -EFAULT
; (see vm_kernel_fault). Neither routine touches the stack before its
; faulting instructions, so the fixup's ret returns to the caller.

EFAULT  equ 14

section .text

global uaccess_start
global uaccess_end
global uaccess_fixup
global uaccess_copy
global uaccess_strncpy

uaccess_start:

; int uaccess_copy(void *dst, const void *src, size_t n)
uaccess_copy:
    mov     rcx, rdx
    rep     movsb
    xor     eax, eax
    ret

; long uaccess_strncpy(char *dst, const char *src, size_t max)
; Returns the string length, max if no NUL was found, or -EFAULT.
uaccess_strncpy:
    xor     eax, eax
.loop:
    cmp     rax, rdx
    jae     .done
    mov     cl, [rsi + rax]
    mov     [rdi + rax], cl
    test    cl, cl
    jz      .done
    inc     rax
    jmp     .loop
.done:
    ret

uaccess_end:

uaccess_fixup:
    mov     rax, -EFAULT
    ret
