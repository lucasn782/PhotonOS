#include <stddef.h>
#include <stdint.h>

#include "ulibc.h"

#define SYS_WRITE 1
#define SYS_EXIT 5

static long syscall1(long number, long arg1)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1)
        : "rcx", "r11", "memory");
    return ret;
}

static long syscall3(long number, long arg1, long arg2, long arg3)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory");
    return ret;
}

static void write_str(const char *str)
{
    syscall3(SYS_WRITE, 1, (long)str, (long)strlen(str));
}

static void on_sigint(int signum)
{
    (void)signum;
    write_str("PhotonOS: Peguei o SIGINT, mas nao vou parar!\n");
    signal(SIGINT, 0);
}

void _start(void)
{
    signal(SIGINT, on_sigint);
    write_str("PhotonOS: hang rodando. Ctrl+C uma vez avisa, duas encerra.\n");

    for (;;) {
        __asm__ volatile ("pause");
    }

    syscall1(SYS_EXIT, 0);
}
