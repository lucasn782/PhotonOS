; PhotonOS 64-bit trampoline.
; Boot sector loads this raw binary at physical 0x8000 and jumps to _start.

global _start
global keyboard_irq_stub
global timer_irq_stub
global mouse_irq_stub
global switch_to
global tss_install
global syscall_entry
global double_fault_stub
global page_fault_stub
global tlb_shootdown_stub
global gpf_stub
global spurious_irq_stub

extern kmain
extern keyboard_irq_handler
extern scheduler_tick
extern syscall_handler
extern syscall_kernel_rsp0
extern double_fault_handler
extern mouse_handler
extern vmm_page_fault_handler
extern smp_tlb_shootdown_handler
extern gpf_handler


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

    ; Write 'VESA' signature to buffer to query VBE 2.0+ info
    mov dword [0x7E00], 0x41534556
    mov ax, 0x4F00
    mov di, 0x7E00
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    ; Query VBE Mode Info for 1024x768x32 LFB
    mov ax, 0x4F01
    mov cx, 0x4118
    mov di, 0x7E00
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    ; Save VBE params
    mov eax, [0x7E00 + 0x28] ; PhysBasePtr
    mov [boot_params], eax
    
    mov ax, [0x7E00 + 0x12]  ; XResolution
    mov [boot_params + 4], ax
    
    mov ax, [0x7E00 + 0x14]  ; YResolution
    mov [boot_params + 6], ax
    
    mov ax, [0x7E00 + 0x10]  ; BytesPerScanLine
    mov [boot_params + 8], ax

    ; Set graphic mode
    mov ax, 0x4F02
    mov bx, 0x4118
    int 0x10
    cmp ax, 0x004F
    je .vbe_ok

.vbe_fail:
    mov ax, 0xB800
    mov es, ax
    mov word [es:0], 0x4F56 ; Red 'V'
.vbe_halt:
    hlt
    jmp .vbe_halt

.vbe_ok:
    xor ax, ax
    mov es, ax

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

mouse_irq_stub:
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
    call mouse_handler
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


double_fault_stub:
    ; IST1 stack frame layout on #DF (CPU pushes these automatically):
    ;   [RSP+0]  = Error Code (always 0 for Double Fault)
    ;   [RSP+8]  = RIP of faulting instruction
    ;   [RSP+16] = CS
    ;   [RSP+24] = RFLAGS
    ;   [RSP+32] = RSP of faulting context
    ;   [RSP+40] = SS
    ;
    ; C handler signature: double_fault_handler(rip, cs, rflags, rsp, ss, error_code)
    ;   rdi = rip, rsi = cs, rdx = rflags, rcx = rsp, r8 = ss, r9 = error_code
    mov rdi, [rsp + 8]   ; arg1: RIP
    mov rsi, [rsp + 16]  ; arg2: CS
    mov rdx, [rsp + 24]  ; arg3: RFLAGS
    mov rcx, [rsp + 32]  ; arg4: faulting RSP
    mov r8,  [rsp + 40]  ; arg5: SS
    mov r9,  [rsp + 0]   ; arg6: Error Code (always 0)

    ; Align RSP to 16-byte boundary before C call (SysV AMD64 ABI requirement)
    mov rbp, rsp
    sub rsp, 8
    and rsp, -16
    cld
    call double_fault_handler

    ; double_fault_handler never returns (infinite hlt loop), but for safety:
    mov rsp, rbp
    add rsp, 8           ; skip error code
    iretq


page_fault_stub:
    ; CPU pushed error code at [rsp], RIP at [rsp+8]
    ; Let's push all registers to preserve them.
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

    mov rdi, [rsp + 120]    ; arg1: error_code
    mov rsi, cr2            ; arg2: fault_addr (CR2)
    mov rdx, [rsp + 128]    ; arg3: faulting RIP
    mov rcx, [rsp + 136]    ; arg4: CS


    mov rbp, rsp
    sub rsp, 8
    and rsp, -16
    cld
    call vmm_page_fault_handler
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

    add rsp, 8              ; skip error code
    iretq


gpf_stub:
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

    mov rdi, [rsp + 120]    ; arg1: error_code
    mov rsi, [rsp + 128]    ; arg2: faulting RIP
    mov rdx, [rsp + 136]    ; arg3: CS

    mov rbp, rsp
    sub rsp, 8
    and rsp, -16
    cld
    call gpf_handler
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
    add rsp, 8              ; skip error code
    iretq


spurious_irq_stub:
    iretq



tlb_shootdown_stub:
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
    sub rsp, 8
    and rsp, -16
    cld
    call smp_tlb_shootdown_handler
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
    ; Fill in the 64-bit TSS descriptor (16 bytes) in the GDT.
    ; RDI = linear address of struct tss64.
    ;
    ; 64-bit TSS System Descriptor layout (16 bytes):
    ;   [+0]  Limit[15:0]
    ;   [+2]  Base[15:0]
    ;   [+4]  Base[23:16]
    ;   [+5]  Access (P=1, DPL=0, Type=0x09 = 64-bit TSS Available)
    ;   [+6]  Flags + Limit[19:16]  (G=0, must be 0)
    ;   [+7]  Base[31:24]
    ;   [+8]  Base[63:32]
    ;   [+12] Reserved (must be 0)
    mov word [gdt_tss], 103         ; Limit = sizeof(tss64)-1 = 103
    mov rax, rdi                    ; RAX = TSS linear address
    mov word [gdt_tss + 2], ax      ; Base[15:0]
    shr rax, 16
    mov byte [gdt_tss + 4], al      ; Base[23:16]
    mov byte [gdt_tss + 5], 10001001b ; P=1, DPL=0, S=0, Type=1001 (TSS64 Available)
    mov byte [gdt_tss + 6], 0       ; G=0, Limit[19:16]=0
    shr rax, 8
    mov byte [gdt_tss + 7], al      ; Base[31:24]
    shr rax, 8
    mov dword [gdt_tss + 8], eax    ; Base[63:32]
    mov dword [gdt_tss + 12], 0     ; Reserved

    ; Reload GDTR so the processor descriptor cache is fully coherent
    ; after the in-place TSS descriptor write.
    lgdt [gdt_descriptor]

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
    dq gdt_start

global boot_params
align 8
boot_params:
    dd 0    ; PhysBasePtr
    dw 0    ; XResolution
    dw 0    ; YResolution
    dw 0    ; BytesPerScanLine
    dw 0    ; padding

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
