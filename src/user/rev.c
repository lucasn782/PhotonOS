#include <stddef.h>
#include <stdint.h>

#include "ulibc.h"

#define SYS_WRITE 1
#define SYS_READ 3
#define SYS_EXIT 5

static inline __attribute__((always_inline)) long user_syscall1(long number,
    long arg1)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline __attribute__((always_inline)) long user_syscall3(long number,
    long arg1, long arg2, long arg3)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory");
    return ret;
}

void _start(void)
{
    char input[256];
    long bytes = user_syscall3(SYS_READ, 0, (long)input, sizeof(input));

    if (bytes > 0) {
        char *output = malloc((size_t)bytes);
        if (output != 0) {
            for (long i = 0; i < bytes; i++) {
                output[i] = input[bytes - i - 1];
            }
            user_syscall3(SYS_WRITE, 1, (long)output, bytes);
            user_syscall3(SYS_WRITE, 1, (long)"\n", 1);
            free(output);
        }
    }

    user_syscall1(SYS_EXIT, 0);

    for (;;) {
        __asm__ volatile ("pause");
    }
}
