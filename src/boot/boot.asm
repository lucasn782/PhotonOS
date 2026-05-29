; PhotonOS stage 1 boot sector.
; Pure NASM, 512 bytes exactly after assembly.
;
; References:
; - Intel 64 and IA-32 SDM, Vol. 3A, "Processor Management and
;   Initialization": reset starts in real-address mode with paging disabled.
; - AMD64 APM Vol. 2, Sec. 14.1.3: after reset CR0=6000_0010h, CR3=0,
;   CR4=0, EFER=0, and the processor is executing 16-bit real-mode code.
; - AMD64 APM Vol. 2, Sec. 14.3: a real-mode environment needs a valid
;   stack before interrupts/exceptions or firmware services can use it.
; - Intel SDM Vol. 3B, Sec. 23.1.1: A20M# can mask address line A20 for
;   8086-compatible wraparound; PhotonOS enables A20 before stage 2.
;
; Mission: enable A20, load the stage-2 payload to 0800:0000, jump there.

[bits 16]
[org 0x7C00]

KERNEL_SEG     equ 0x0800
KERNEL_SEG2    equ 0x1800
KERNEL_OFF     equ 0x0000
KERNEL_SECTORS equ 256
KERNEL_FIRST_SECTORS equ 128
KERNEL_SECOND_SECTORS equ KERNEL_SECTORS - KERNEL_FIRST_SECTORS
SECTORS_TRACK  equ 18

start:
    cli                         ; IF=0 while SS:SP is being rebuilt.
    xor ax, ax                  ; AX=0 for real-mode data and stack bases.
    mov ds, ax                  ; DS=0: boot-sector labels are at 0000:7C00.
    mov ss, ax                  ; SS=0: stack physical base is 00000h.
    mov sp, 0x7C00              ; SP=7C00h: stack grows below the boot sector.
    cld                         ; DF=0: any string ops move upward.
    sti                         ; IF=1: BIOS disk service may rely on IRQs.

    mov [boot_drive], dl        ; Preserve BIOS boot drive number.
    xor ah, ah                  ; AH=00h: reset disk system.
    push ds                     ; Firmware calls are not trusted with DS.
    int 0x13                    ; BIOS int 13h reset for DL.
    pop ds                      ; DS=0 again for boot-sector data.

    call enable_a20             ; Enable address line A20 before stage 2.
    call load_kernel_lba        ; Prefer EDD LBA read for the full payload.
    jc load_kernel_chs          ; If unavailable, fall back to floppy CHS.

jump_kernel:
    cli                         ; Kernel owns interrupt policy from here.
    jmp KERNEL_SEG:KERNEL_OFF   ; CS:IP=0800:0000, physical 00008000h.

enable_a20:
    mov ax, 0x2401              ; BIOS INT 15h: request A20 enable.
    push ds                     ; Firmware calls are not trusted with DS.
    int 0x15                    ; CF may report unsupported; fast A20 follows.
    pop ds                      ; DS=0 again for boot-sector data.
    in al, 0x92                 ; AL=system control port A.
    or al, 00000010b            ; Set bit 1: A20 enable.
    and al, 11111110b           ; Clear bit 0: avoid fast reset.
    out 0x92, al                ; Commit fast-A20 request.
    ret                         ; Stage 2 can now rely on non-wrapping memory.

load_kernel_lba:
    mov ah, 0x41                ; AH=41h: check int 13h extensions.
    mov bx, 0x55AA              ; BX=signature required by EDD probe.
    mov dl, [boot_drive]        ; DL=BIOS boot drive.
    push ds                     ; Preserve DS across firmware.
    int 0x13                    ; CF=0 and BX=AA55h means extensions present.
    pop ds                      ; DS=0 for DAP/variables.
    jc .fail                    ; No EDD on this drive.
    cmp bx, 0xAA55              ; BIOS returns AA55h when EDD is supported.
    jne .fail                   ; Wrong signature: do not trust AH=42h.
    test cx, 0x0001             ; CX bit 0=extended disk access functions.
    jz .fail                    ; AH=42h packet read unavailable.

    mov si, disk_packet         ; DS:SI points to the Disk Address Packet.
    mov ah, 0x42                ; AH=42h: extended read.
    mov dl, [boot_drive]        ; DL=BIOS boot drive.
    push ds                     ; Preserve DS across firmware.
    int 0x13                    ; Read stage-2 payload from LBA 1 to 0800:0000.
    pop ds                      ; DS=0 again.
    jc .fail

    mov si, disk_packet2        ; Load the remainder past the 64K boundary.
    mov ah, 0x42
    mov dl, [boot_drive]
    push ds
    int 0x13
    pop ds
    ret                         ; CF tells caller success/failure.
.fail:
    stc                         ; CF=1: use CHS fallback.
    ret                         ; Return failure to caller.

load_kernel_chs:
    xor ax, ax                  ; AX=0.
    mov ds, ax                  ; DS=0 for boot-sector variables.
    mov ax, KERNEL_SEG          ; AX=destination segment 0800h.
    mov es, ax                  ; ES:BX is BIOS read buffer.
    xor bx, bx                  ; BX=0000h: destination offset.
    mov si, KERNEL_SECTORS      ; SI=number of sectors still to read.
    xor ch, ch                  ; CH=cylinder 0.
    xor dh, dh                  ; DH=head 0.
    mov cl, 2                   ; CL=sector 2, immediately after boot sector.

.read_one:
    push cx                     ; BIOS should preserve CHS, but be defensive.
    push dx                     ; Preserve DH=head while DL is reloaded.
    push ds                     ; Preserve DS for [boot_drive].
    mov ax, 0x0201              ; AH=02h read sectors, AL=1 sector.
    mov dl, [boot_drive]        ; DL=BIOS boot drive.
    int 0x13                    ; Read CHS sector into ES:BX.
    pop ds                      ; Restore DS=0.
    pop dx                      ; Restore DH=head.
    pop cx                      ; Restore CH=cyl, CL=sector.
    jc disk_error               ; CF=1: disk read failed.

    add bx, 512                 ; Advance destination by one sector.
    jnc .advance_chs            ; If BX did not wrap, ES stays the same.
    mov ax, es                  ; BIOS buffers cannot cross a 64K window.
    add ax, 0x1000              ; Move ES forward by 64 KiB.
    mov es, ax                  ; ES:BX remains a contiguous physical buffer.
.advance_chs:
    inc cl                      ; Next sector number on current track.
    cmp cl, SECTORS_TRACK + 1   ; Past sector 18 on 1.44M floppy geometry?
    jb .next                    ; Still on same track.
    mov cl, 1                   ; Wrap sector number to 1.
    xor dh, 1                   ; Toggle head 0 <-> 1.
    jnz .next                   ; If now head 1, same cylinder.
    inc ch                      ; After head 1 wraps to 0, next cylinder.
.next:
    dec si                      ; One fewer sector to read.
    jnz .read_one               ; Continue until all payload sectors are loaded.
    jmp jump_kernel             ; Loaded through CHS path.

disk_error:
    cli                         ; Stop maskable IRQs before parking.
    mov ax, 0xB800              ; AX=VGA color text segment.
    mov es, ax                  ; ES base becomes physical B8000h.
    mov word [es:0], 0x4F45     ; Show 'E' if disk read fails.
.halt:
    hlt                         ; Wait for reset/NMI/SMI.
    jmp .halt                   ; If resumed, park again.

boot_drive db 0

align 4, db 0
disk_packet:
    db 0x10                     ; DAP size = 16 bytes.
    db 0x00                     ; Reserved, must be zero.
    dw KERNEL_FIRST_SECTORS     ; Number of sectors to read.
    dw KERNEL_OFF               ; Destination offset.
    dw KERNEL_SEG               ; Destination segment.
    dq 0x0000000000000001       ; Starting LBA: sector immediately after boot.

disk_packet2:
    db 0x10
    db 0x00
    dw KERNEL_SECOND_SECTORS
    dw KERNEL_OFF
    dw KERNEL_SEG2
    dq 0x0000000000000081

times 510 - ($ - $$) db 0       ; Pad to byte offset 510.
dw 0xAA55                       ; BIOS boot signature: bytes 55 AA on disk.
