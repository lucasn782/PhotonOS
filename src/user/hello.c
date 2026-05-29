#include <stddef.h>
#include <stdint.h>

#define SYS_EXIT 5
#define SYS_WRITE 1

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

static size_t strlen(const char *str)
{
    size_t len = 0;

    while (str[len] != '\0') {
        len++;
    }

    return len;
}

void _start(void)
{
    const char *msg = "PhotonOS: Ola do espaco de usuario!\n";

    syscall3(SYS_WRITE, 1, (long)msg, (long)strlen(msg));
    syscall1(SYS_EXIT, 0);

    for (;;) {
        __asm__ volatile ("pause");
    }
}
