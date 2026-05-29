#include <stddef.h>
#include <stdint.h>

#define SYS_WRITE 1
#define SYS_READ 3
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

static char upper_char(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        return (char)(ch - 'a' + 'A');
    }

    return ch;
}

void _start(void)
{
    char buffer[128];
    long bytes = syscall3(SYS_READ, 0, (long)buffer, sizeof(buffer));

    if (bytes > 0) {
        for (long i = 0; i < bytes; i++) {
            buffer[i] = upper_char(buffer[i]);
        }
        syscall3(SYS_WRITE, 1, (long)buffer, bytes);
        syscall3(SYS_WRITE, 1, (long)"\n", 1);
    }

    syscall1(SYS_EXIT, 0);

    for (;;) {
        __asm__ volatile ("pause");
    }
}
