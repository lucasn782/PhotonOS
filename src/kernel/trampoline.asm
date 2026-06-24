[bits 16]
org 0x7000

entry:
    jmp real_start

    ; Fixed layout at offset 0x10 for parameter passing from BSP (Vector D)
    align 16
ap_cr3:      dq 0   ; 0x7010
ap_rsp:      dq 0   ; 0x7018
ap_id:       dq 0   ; 0x7020
ap_ready:    dq 0   ; 0x7028
ap_entry:    dq 0   ; 0x7030

real_start:
    cli
    cld

    ; Initialize standard segments to 0
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Far jump to reload CS to 0, ensuring segment offset consistency
    jmp 0:.zero_cs

.zero_cs:
    ; Load the temporary 32-bit/64-bit GDT descriptor
    lgdt [gdt_ptr]

    ; Enable 32-bit Protected Mode by setting PE bit of CR0
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Execute a 32-bit far jump to enter Protected Mode (Selector 8)
    jmp dword 8:protected_entry

[bits 32]
protected_entry:
    ; Reload all data segments to Protected Mode data selector (Selector 16)
    mov ax, 16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Setup temporary 32-bit stack pointer at 0x7000 (safe region growing down)
    mov esp, 0x7000

    ; Set PAE bit in CR4 for Long Mode paging
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Load Kernel PML4 Page Directory Address (CR3) passed by BSP at 0x7010
    mov eax, [0x7010]
    mov cr3, eax

    ; Enable IA32_EFER.LME (Long Mode Enable) using MSR 0xC0000080
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable paging by setting PG (bit 31) and PE (bit 0) of CR0
    mov eax, cr0
    or eax, 0x80000001
    mov cr0, eax

    ; Far jump to 64-bit Long Mode using selector 24 (0x18)
    jmp dword 24:long_mode_entry

[bits 64]
default abs
long_mode_entry:
    ; Clean up segment registers for 64-bit mode (use Selector 16 or 0)
    mov ax, 16
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Load stack pointer from ap_rsp parameter (offset 0x7018) and align
    mov rsp, [0x7018]
    and rsp, -16

    ; Pass ap_id as the first parameter (RDI in SysV AMD64) from offset 0x7020
    mov rdi, [0x7020]

    ; Notify BSP that this AP is ready by setting ready to 1 (offset 0x7028)
    mov qword [0x7028], 1

    ; Indirect jump to the kernel's C entry function (offset 0x7030)
    mov rax, [0x7030]
    jmp rax

align 16
gdt_start:
    dq 0 ; null descriptor

gdt_code32:
    dw 0xFFFF       ; Limit 15:0
    dw 0x0000       ; Base 15:0
    db 0x00         ; Base 23:16
    db 10011010b    ; Access: P=1, DPL=0, S=1, Type=Code Execute-Read
    db 11001111b    ; Flags: G=1, D=1 (32-bit), Limit 19:16 = 0xF
    db 0x00         ; Base 31:24

gdt_data32:
    dw 0xFFFF       ; Limit 15:0
    dw 0x0000       ; Base 15:0
    db 0x00         ; Base 23:16
    db 10010010b    ; Access: P=1, DPL=0, S=1, Type=Data Read-Write
    db 11001111b    ; Flags: G=1, B=1 (32-bit), Limit 19:16 = 0xF
    db 0x00         ; Base 31:24

gdt_code64:
    dw 0xFFFF
    dw 0
    db 0
    db 10011010b    ; Access: P=1, DPL=0, S=1, Type=Code Execute-Read
    db 10101111b    ; Flags: L=1 (64-bit code)
    db 0

gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start
