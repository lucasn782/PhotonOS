.text
.global _start
.type _start, @function
.extern crt0_main
.extern __crt0_entry_rsp_mod16
_start:
    mov %rsp, %rax
    and $15, %rax
    mov %rax, __crt0_entry_rsp_mod16(%rip)
    sub $8, %rsp
    call crt0_main
    ud2
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
