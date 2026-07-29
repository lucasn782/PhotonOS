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

    /*
     * Persistent e1000 state cannot live in the late .bss, which extends
     * into the legacy VGA aperture at 0xA0000-0xBFFFF.  This NOLOAD section
     * is regular RAM, is mapped by the bootstrap identity map, and is kept
     * inside the PMM reserved range through __kernel_end below.
     */
    . = 0x00100000;
    .network_state (NOLOAD) :
    {
        *(.network_state)
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
