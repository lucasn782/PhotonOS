#include <stdint.h>

void _start(void)
{
    for (;;) {
        __asm__ volatile ("pause");
    }
}
