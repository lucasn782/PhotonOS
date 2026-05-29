; PhotonOS VGA fallback boot sector.
; Purpose: verify QEMU video output without BIOS int 10h or mode switching.
;
; AMD64 APM Vol. 2, Sec. 14.1.3 says RESET/INIT leave the CPU in real mode,
; with CR0=60000010h, CR3=0, CR4=0, EFER=0, and 16-bit segments. This sector
; stays inside that reset-compatible real-mode environment.

[bits 16]
[org 0x7C00]

start:
    cli                         ; IF=0: no external IRQs while stack is rebuilt.
    xor ax, ax                  ; AX=0 for data and stack segment bases.
    mov ds, ax                  ; DS=0 for any local data references.
    mov ss, ax                  ; SS=0: stack base at physical 00000h.
    mov sp, 0x7C00              ; SP=7C00h: stack grows below this sector.
    cld                         ; DF=0: string writes increment DI.

    mov ax, 0xB800              ; AX=VGA color text segment base >> 4.
    mov es, ax                  ; ES base becomes physical B8000h.
    xor di, di                  ; DI=0: first text cell at B800:0000.
    mov ax, 0x0720              ; AH=07h attribute, AL=20h space.
    mov cx, 80 * 25             ; CX=2000 text cells on a standard VGA page.
    rep stosw                   ; Clear screen with space/attribute words.

    xor di, di                  ; DI=0: top-left screen cell again.
    mov ax, 0x0F21              ; AH=0Fh bright white, AL='!'.
    mov [es:di], ax             ; Write '!' directly to physical B8000h.

.halt:
    hlt                         ; Halt until reset/NMI/SMI.
    jmp .halt                   ; If resumed, park again.

times 510 - ($ - $$) db 0       ; Pad to boot-sector signature offset.
dw 0xAA55                       ; BIOS signature bytes are 55 AA on disk.
