; PhotonOS 64-bit trampoline.
; Boot sector loads this raw binary at physical 0x8000 and jumps to _start.

global _start
global keyboard_irq_stub
global timer_irq_stub
global switch_to
global tss_install
global syscall_entry
extern kmain
extern keyboard_irq_handler
extern scheduler_tick
extern syscall_handler
extern syscall_kernel_rsp0

KERNEL_BASE equ 0x00008000

CODE64_SEL  equ gdt_code64 - gdt_start
DATA_SEL    equ gdt_data   - gdt_start
CODE32_SEL  equ gdt_code32 - gdt_start
USER_DATA_SEL equ gdt_user_data - gdt_start
USER_CODE_SEL equ gdt_user_code - gdt_start
TSS_SEL     equ gdt_tss - gdt_start
SYS_SIGRETURN equ 12

PAGE_FLAGS  equ 0x00000003
IA32_EFER   equ 0xC0000080
IDENTITY_PT_COUNT equ 64
IDENTITY_PAGES    equ IDENTITY_PT_COUNT * 512
PAGE_TABLE_BYTES  equ (3 + IDENTITY_PT_COUNT) * 4096

section .text

[bits 16]
_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 0x00000001
    mov cr0, eax
    jmp dword CODE32_SEL:protected_start

[bits 32]
protected_start:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x00090000

    call setup_page_tables
    call enter_long_mode

setup_page_tables:
    cld
    mov edi, pml4
    xor eax, eax
    mov ecx, PAGE_TABLE_BYTES / 4
    rep stosd

    mov dword [pml4], pdpt + PAGE_FLAGS
    mov dword [pml4 + 4], 0
    mov dword [pdpt], pd + PAGE_FLAGS
    mov dword [pdpt + 4], 0

    mov edi, pd
    mov eax, pts + PAGE_FLAGS
    mov ecx, IDENTITY_PT_COUNT
.map_page_tables:
    mov dword [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x1000
    add edi, 8
    loop .map_page_tables

    mov edi, pts
    xor eax, eax
    mov ecx, IDENTITY_PAGES
.map_128mib:
    mov edx, eax
    or edx, PAGE_FLAGS
    mov dword [edi], edx
    mov dword [edi + 4], 0
    add eax, 0x1000
    add edi, 8
    loop .map_128mib
    ret

enter_long_mode:
    cli

    mov eax, cr0
    and eax, 0x7FFFFFFF
    mov cr0, eax

    mov eax, cr4
    or eax, 0x00000020
    mov cr4, eax

    mov eax, pml4
    mov cr3, eax

    mov ecx, IA32_EFER
    rdmsr
    or eax, 0x00000100
    wrmsr

    mov eax, cr0
    or eax, 0x80000001
    mov cr0, eax
    jmp CODE64_SEL:long_mode_start

[bits 64]
default abs
long_mode_start:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, kernel_stack_top
    and rsp, -16

    call kmain

.halt:
    hlt
    jmp .halt

keyboard_irq_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rbp, rsp
    and rsp, -16
    cld
    call keyboard_irq_handler
    mov rsp, rbp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

timer_irq_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    mov rbp, rsp
    and rsp, -16
    cld
    call scheduler_tick
    mov rsp, rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

switch_to:
    pushfq
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, rsi

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    popfq
    ret

tss_install:
    mov word [gdt_tss], 103
    mov rax, rdi
    mov word [gdt_tss + 2], ax
    shr rax, 16
    mov byte [gdt_tss + 4], al
    mov byte [gdt_tss + 5], 10001001b
    mov byte [gdt_tss + 6], 0
    shr rax, 8
    mov byte [gdt_tss + 7], al
    shr rax, 8
    mov dword [gdt_tss + 8], eax
    mov dword [gdt_tss + 12], 0

    mov ax, TSS_SEL
    ltr ax
    ret

syscall_entry:
    mov [syscall_saved_user_rsp], rsp
    mov [syscall_saved_number], rax
    mov rsp, [syscall_kernel_rsp0]
    test rsp, rsp
    jnz .stack_ready
    mov rsp, syscall_stack_top
.stack_ready:
    and rsp, -16

    push qword [syscall_saved_user_rsp]
    push rcx
    push r11
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9

    mov r9, r8
    mov r8, r10
    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax

    mov rax, rsp
    sub rsp, 16
    mov [rsp], rax
    mov rax, [syscall_saved_number]
    mov [rsp + 8], rax
    cld
    call syscall_handler
    mov r10, [rsp + 8]
    add rsp, 16

    cmp r10, SYS_SIGRETURN
    jne .normal_return
    test rax, rax
    jz .normal_return

    mov rsp, rax
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

.normal_return:
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11
    pop rcx
    pop rsp
    o64 sysret

align 8
gdt_start:
    dq 0

gdt_code64:
    dw 0xFFFF
    dw 0
    db 0
    db 10011010b
    db 10101111b
    db 0

gdt_data:
    dw 0xFFFF
    dw 0
    db 0
    db 10010010b
    db 11001111b
    db 0

gdt_code32:
    dw 0xFFFF
    dw 0
    db 0
    db 10011010b
    db 11001111b
    db 0

gdt_user_data:
    dw 0xFFFF
    dw 0
    db 0
    db 11110010b
    db 11001111b
    db 0

gdt_user_code:
    dw 0xFFFF
    dw 0
    db 0
    db 11111010b
    db 10101111b
    db 0

gdt_tss:
    dq 0
    dq 0

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

section .bss
align 4096
pml4:
    resb 4096

align 4096
pdpt:
    resb 4096

align 4096
pd:
    resb 4096

align 4096
pts:
    resb IDENTITY_PT_COUNT * 4096

align 16
kernel_stack:
    resb 8192
kernel_stack_top:

align 16
syscall_stack:
    resb 8192
syscall_stack_top:

align 8
syscall_saved_user_rsp:
    resq 1

align 8
syscall_saved_number:
    resq 1
