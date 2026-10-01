bits 64

extern main
extern exit
extern __libc_init

section .text

global _start
_start:
    xor     ebp, ebp
    mov     rdi, [rsp]
    lea     rsi, [rsp + 8]
    lea     rdx, [rsi + rdi * 8 + 8]
    and     rsp, -16
    push    rdi
    push    rsi
    push    rdx
    sub     rsp, 8
    mov     rdi, rdx
    call    __libc_init
    add     rsp, 8
    pop     rdx
    pop     rsi
    pop     rdi
    call    main
    mov     edi, eax
    call    exit
    ud2

; Signal handlers return here; the kernel restores the interrupted state.
global __signal_restorer
__signal_restorer:
    mov     eax, 64                 ; SYS_SIGRETURN
    syscall
    ud2
