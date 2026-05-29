ENTRY(_start)

SECTIONS
{
    . = 0x00008000;

    .text ALIGN(4096) :
    {
        *(.text*)
    }

    .rodata ALIGN(4096) :
    {
        *(.rodata*)
    }

    .data ALIGN(4096) :
    {
        *(.data*)
    }

    .bss ALIGN(4096) :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        __bss_end = .;
    }

    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
