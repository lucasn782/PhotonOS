#include "ulibc.h"

#include <stdarg.h>
#include <stdint.h>

#include "sys/socket.h"

#define SYS_WRITE 1
#define SYS_READ 3
#define SYS_EXIT 5
#define SYS_BRK 9
#define SYS_SIGNAL 10
#define SYS_KILL 11
#define SYS_SIGRETURN 12
#define SYS_GETPROCS 13
#define SYS_CLOSE 15
#define SYS_SOCKET_SEND 17
#define SYS_SOCKET_RECV 18
#define SYS_YIELD 19
#define SYS_GET_TICKS 20
#define SYS_READDIR 21
#define SYS_SOCKET 24
#define SYS_BIND 25
#define PAGE_SIZE 4096UL

struct malloc_block {
    size_t size;
    int free;
    struct malloc_block *next;
};

static struct malloc_block *heap_head;
static struct malloc_block *heap_tail;

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

static long syscall2(long number, long arg1, long arg2)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2)
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

static long syscall4(long number, long arg1, long arg2, long arg3, long arg4)
{
    long ret;
    register long r10 __asm__("r10") = arg4;

    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10)
        : "rcx", "r11", "memory");
    return ret;
}

static long syscall0(long number)
{
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(number)
        : "rcx", "r11", "memory");
    return ret;
}

static size_t align16(size_t value)
{
    return (value + 15UL) & ~15UL;
}

static size_t align_page(size_t value)
{
    return (value + PAGE_SIZE - 1UL) & ~(PAGE_SIZE - 1UL);
}

static struct malloc_block *find_free_block(size_t size)
{
    for (struct malloc_block *block = heap_head; block != 0;
        block = block->next) {
        if (block->free && block->size >= size) {
            return block;
        }
    }

    return 0;
}

static struct malloc_block *extend_heap(size_t size)
{
    uintptr_t current = (uintptr_t)syscall1(SYS_BRK, 0);
    uintptr_t new_end = current + align_page(sizeof(struct malloc_block) + size);
    uintptr_t result = (uintptr_t)syscall1(SYS_BRK, (long)new_end);

    if (result < new_end) {
        return 0;
    }

    struct malloc_block *block = (struct malloc_block *)current;
    block->size = result - current - sizeof(struct malloc_block);
    block->free = 0;
    block->next = 0;

    if (heap_tail != 0) {
        heap_tail->next = block;
    } else {
        heap_head = block;
    }
    heap_tail = block;

    return block;
}

void *malloc(size_t size)
{
    if (size == 0) {
        return 0;
    }

    size = align16(size);
    struct malloc_block *block = find_free_block(size);
    if (block == 0) {
        block = extend_heap(size);
    }
    if (block == 0) {
        return 0;
    }

    block->free = 0;
    return (void *)(block + 1);
}

void free(void *ptr)
{
    if (ptr == 0) {
        return;
    }

    struct malloc_block *block = ((struct malloc_block *)ptr) - 1;
    block->free = 1;
}

size_t strlen(const char *str)
{
    size_t length = 0;

    while (str[length] != '\0') {
        length++;
    }

    return length;
}

void *memcpy(void *dest, const void *src, size_t size)
{
    uint8_t *out = dest;
    const uint8_t *in = src;

    for (size_t i = 0; i < size; i++) {
        out[i] = in[i];
    }

    return dest;
}

static int stdout_write(const char *str, size_t len)
{
    return (int)syscall3(SYS_WRITE, 1, (long)str, (long)len);
}

static int printf_emit_char(char ch)
{
    stdout_write(&ch, 1);
    return 1;
}

static int printf_emit_string(const char *str)
{
    if (str == 0) {
        str = "(null)";
    }

    size_t len = strlen(str);
    stdout_write(str, len);
    return (int)len;
}

static int printf_emit_unsigned(unsigned long value, unsigned int base,
    int uppercase)
{
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char buffer[32];
    size_t len = 0;
    int written = 0;

    if (value == 0) {
        return printf_emit_char('0');
    }

    while (value != 0 && len < sizeof(buffer)) {
        buffer[len++] = digits[value % base];
        value /= base;
    }

    while (len > 0) {
        written += printf_emit_char(buffer[--len]);
    }

    return written;
}

int printf(const char *format, ...)
{
    va_list args;
    int written = 0;

    if (format == 0) {
        return 0;
    }

    va_start(args, format);
    while (*format != '\0') {
        if (*format != '%') {
            written += printf_emit_char(*format++);
            continue;
        }

        format++;
        int long_arg = 0;
        int size_arg = 0;
        if (*format == 'l') {
            long_arg = 1;
            format++;
        } else if (*format == 'z') {
            size_arg = 1;
            format++;
        }

        char spec = *format++;
        if (spec == '\0') {
            break;
        }

        if (spec == '%') {
            written += printf_emit_char('%');
        } else if (spec == 'c') {
            written += printf_emit_char((char)va_arg(args, int));
        } else if (spec == 's') {
            written += printf_emit_string(va_arg(args, const char *));
        } else if (spec == 'd' || spec == 'i') {
            long value = size_arg ? (long)va_arg(args, size_t) :
                (long_arg ? va_arg(args, long) : va_arg(args, int));
            if (value < 0) {
                written += printf_emit_char('-');
                value = -value;
            }
            written += printf_emit_unsigned((unsigned long)value, 10, 0);
        } else if (spec == 'u' || spec == 'x' || spec == 'X') {
            unsigned long value = size_arg ? (unsigned long)va_arg(args, size_t) :
                (long_arg ? va_arg(args, unsigned long) :
                    va_arg(args, unsigned int));
            written += printf_emit_unsigned(value,
                spec == 'u' ? 10U : 16U, spec == 'X');
        } else if (spec == 'p') {
            unsigned long value = (unsigned long)va_arg(args, void *);
            written += printf_emit_string("0x");
            written += printf_emit_unsigned(value, 16, 0);
        } else {
            written += printf_emit_char('%');
            written += printf_emit_char(spec);
        }
    }
    va_end(args);

    return written;
}

void exit(int status)
{
    syscall1(SYS_EXIT, status);
    for (;;) {
        __asm__ volatile ("pause");
    }
}

void yield(void)
{
    syscall0(SYS_YIELD);
}

sighandler_t signal(int signum, sighandler_t handler)
{
    return (sighandler_t)syscall2(SYS_SIGNAL, signum, (long)handler);
}

int kill(int pid, int signum)
{
    return (int)syscall2(SYS_KILL, pid, signum);
}

void sigreturn(void)
{
    syscall0(SYS_SIGRETURN);
    for (;;) {
        __asm__ volatile ("pause");
    }
}

int getprocs(proc_info_t *buffer, size_t max_size)
{
    return (int)syscall2(SYS_GETPROCS, (long)buffer, (long)max_size);
}

uint64_t get_ticks(void)
{
    return (uint64_t)syscall0(SYS_GET_TICKS);
}

int readdir(int fd, vfs_dir_entry_t *buf, uint32_t count)
{
    return (int)syscall3(SYS_READDIR, (long)fd, (long)buf, (long)count);
}

int socket_send(uint32_t dest_ip, uint8_t protocol, const void *payload,
    size_t len)
{
    return (int)syscall4(SYS_SOCKET_SEND, (long)dest_ip, (long)protocol,
        (long)payload, (long)len);
}

int socket_recv(uint8_t protocol, void *buffer, size_t max_len)
{
    return (int)syscall3(SYS_SOCKET_RECV, (long)protocol, (long)buffer,
        (long)max_len);
}

int read(int fd, void *buf, size_t count)
{
    return (int)syscall3(SYS_READ, fd, (long)buf, (long)count);
}

int write(int fd, const void *buf, size_t count)
{
    return (int)syscall3(SYS_WRITE, fd, (long)buf, (long)count);
}

int close(int fd)
{
    return (int)syscall1(SYS_CLOSE, fd);
}

int socket(int domain, int type, int protocol)
{
    return (int)syscall3(SYS_SOCKET, domain, type, protocol);
}

int bind(int fd, const struct sockaddr *addr, uint32_t addrlen)
{
    return (int)syscall3(SYS_BIND, fd, (long)addr, addrlen);
}

static int inet_is_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

uint32_t inet_addr(const char *ip_str)
{
    uint32_t octets[4];
    uint32_t value = 0;
    int octet = 0;
    int saw_digit = 0;

    if (ip_str == 0) {
        return 0;
    }

    for (size_t i = 0;; i++) {
        char ch = ip_str[i];

        if (inet_is_digit(ch)) {
            saw_digit = 1;
            value = (value * 10U) + (uint32_t)(ch - '0');
            if (value > 255U) {
                return 0;
            }
            continue;
        }

        if (ch == '.' || ch == '\0') {
            if (!saw_digit || octet >= 4) {
                return 0;
            }

            octets[octet++] = value;
            value = 0;
            saw_digit = 0;

            if (ch == '\0') {
                break;
            }
            continue;
        }

        return 0;
    }

    if (octet != 4) {
        return 0;
    }

    return htonl((octets[0] << 24) | (octets[1] << 16) |
        (octets[2] << 8) | octets[3]);
}
