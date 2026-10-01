; Aegis Boot Manager - stage 1 (BIOS boot sector)
;
; Loaded by the BIOS at 0000:7C00. Responsibilities:
;   1. Normalise segments and set up a real-mode stack.
;   2. Load the rest of the boot manager (stage 2, the C code) from LBA 1
;      to 0000:7E00 using INT 13h extensions.
;   3. Enable the A20 line.
;   4. Switch to 32-bit protected mode with a flat GDT.
;   5. Zero .bss and call btmngr_entry(boot_drive).
;
; The stage 2 sector count is computed by the linker (see linker.ld).

bits 16

extern btmngr_entry
extern __btmngr_sectors
extern __bss_start
extern __bss_end

STAGE2_SEG      equ 0x07E0          ; 07E0:0000 == 0x7E00 linear
PM_STACK_TOP    equ 0x00090000

CODE_SEL        equ gdt_code - gdt_start
DATA_SEL        equ gdt_data - gdt_start

section .boot progbits alloc exec

global boot_start
boot_start:
    jmp     0x0000:.normalise       ; force CS = 0

.normalise:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00              ; stack grows down below the boot sector
    sti
    cld

    mov     [boot_drive], dl        ; BIOS passes the boot drive in DL

    ; Check for INT 13h extensions (LBA reads).
    mov     ah, 0x41
    mov     bx, 0x55AA
    int     0x13
    jc      .no_lba
    cmp     bx, 0xAA55
    jne     .no_lba

    ; Load stage 2.
    mov     word [dap_count], __btmngr_sectors
    mov     si, dap
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jc      .disk_error

    ; Enable A20: try the BIOS first, then fall back to the fast A20 gate.
    mov     ax, 0x2401
    int     0x15
    in      al, 0x92
    test    al, 2
    jnz     .a20_done
    or      al, 2
    and     al, 0xFE                ; never set bit 0 (fast reset)
    out     0x92, al
.a20_done:

    ; Enter protected mode.
    cli
    lgdt    [gdt_desc]
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax
    jmp     dword CODE_SEL:pm_entry

.no_lba:
    mov     si, msg_no_lba
    jmp     fatal
.disk_error:
    mov     si, msg_disk
    ; fall through

; Print the NUL-terminated string at DS:SI and halt.
fatal:
    mov     ah, 0x0E
    xor     bx, bx
.next:
    lodsb
    test    al, al
    jz      .halt
    int     0x10
    jmp     .next
.halt:
    cli
    hlt
    jmp     .halt

bits 32
pm_entry:
    mov     ax, DATA_SEL
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     esp, PM_STACK_TOP

    ; Zero .bss.
    mov     edi, __bss_start
    mov     ecx, __bss_end
    sub     ecx, edi
    xor     eax, eax
    rep     stosb

    movzx   eax, byte [boot_drive]
    push    eax
    call    btmngr_entry

.hang:
    cli
    hlt
    jmp     .hang

boot_drive: db 0

align 4
dap:                                ; INT 13h AH=42h disk address packet
    db      0x10                    ; packet size
    db      0
dap_count:
    dw      0                       ; sectors to read (patched at runtime)
    dw      0x0000                  ; buffer offset
    dw      STAGE2_SEG              ; buffer segment
    dq      1                       ; starting LBA

align 8
gdt_start:
    dq      0                       ; null descriptor
gdt_code:                           ; 0x08: base 0, limit 4 GiB, 32-bit code
    dw      0xFFFF, 0x0000
    db      0x00, 10011010b, 11001111b, 0x00
gdt_data:                           ; 0x10: base 0, limit 4 GiB, 32-bit data
    dw      0xFFFF, 0x0000
    db      0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_desc:
    dw      gdt_end - gdt_start - 1
    dd      gdt_start

msg_no_lba: db "Aegis: BIOS lacks LBA support", 0
msg_disk:   db "Aegis: disk read error", 0

    times 446 - ($ - $$) db 0
    times 64 db 0
    dw      0xAA55
