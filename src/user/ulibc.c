#include "ulibc.h"
#include "string.h"
#include "stdio.h"

#include <stdarg.h>
#include <stdint.h>

#include "sys/socket.h"

#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_READ 3
#define SYS_SPAWN 4
#define SYS_EXIT 5
#define SYS_CREATE 6
#define SYS_WAIT 7
#define SYS_PIPE 8
#define SYS_BRK 9
#define SYS_SIGNAL 10
#define SYS_KILL 11
#define SYS_SIGRETURN 12
#define SYS_GETPROCS 13
#define SYS_DUP2 14
#define SYS_CLOSE 15
#define SYS_LIST 16
#define SYS_SOCKET_SEND 17
#define SYS_SOCKET_RECV 18
#define SYS_YIELD 19
#define SYS_GET_TICKS 20
#define SYS_READDIR 21
#define SYS_EXECVE 22
#define SYS_FORK 23
#define SYS_SOCKET 24
#define SYS_BIND 25
#define SYS_CONNECT 26
#define SYS_CHMOD 27
#define SYS_CHOWN 28
#define SYS_LINK 29
#define SYS_UNLINK 30
#define SYS_SYMLINK 31
#define SYS_READLINK 32
#define SYS_MOUNT 33
#define SYS_UMOUNT 34
#define SYS_LISTEN 35
#define SYS_ACCEPT 36

#define PAGE_SIZE 4096UL
#define PRINTF_BUF_SIZE 2048

static inline long _syscall(long num, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static long syscall0(long number)
{
    return _syscall(number, 0, 0, 0, 0, 0, 0);
}

static long syscall1(long number, long arg1)
{
    return _syscall(number, arg1, 0, 0, 0, 0, 0);
}

static long syscall2(long number, long arg1, long arg2)
{
    return _syscall(number, arg1, arg2, 0, 0, 0, 0);
}

static long syscall3(long number, long arg1, long arg2, long arg3)
{
    return _syscall(number, arg1, arg2, arg3, 0, 0, 0);
}

static long syscall4(long number, long arg1, long arg2, long arg3, long arg4)
{
    return _syscall(number, arg1, arg2, arg3, arg4, 0, 0);
}

/* -------------------------------------------------------------
 * LOCAL MEMORY ALLOCATOR (Ring 3 malloc/free)
 * -------------------------------------------------------------
 */

struct malloc_block {
    size_t size;
    int free;
    struct malloc_block *next;
};

static struct malloc_block *heap_head = 0;
static struct malloc_block *heap_tail = 0;

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
    for (struct malloc_block *block = heap_head; block != 0; block = block->next) {
        if (block->free && block->size >= size) {
            return block;
        }
    }
    return 0;
}

static void coalesce_free_blocks(void)
{
    struct malloc_block *curr = heap_head;
    while (curr != 0 && curr->next != 0) {
        if (curr->free && curr->next->free) {
            uintptr_t curr_end = (uintptr_t)curr + sizeof(struct malloc_block) + curr->size;
            if (curr_end == (uintptr_t)curr->next) {
                curr->size += sizeof(struct malloc_block) + curr->next->size;
                curr->next = curr->next->next;
                if (curr->next == 0) {
                    heap_tail = curr;
                }
                continue; // check again with the new next block
            }
        }
        curr = curr->next;
    }
}

static void split_block(struct malloc_block *block, size_t size)
{
    size_t min_split = sizeof(struct malloc_block) + 16;
    if (block->size >= size + min_split) {
        struct malloc_block *new_block = (struct malloc_block *)((uintptr_t)block + sizeof(struct malloc_block) + size);
        new_block->size = block->size - size - sizeof(struct malloc_block);
        new_block->free = 1;
        new_block->next = block->next;
        
        block->size = size;
        block->next = new_block;
        
        if (block == heap_tail) {
            heap_tail = new_block;
        }
    }
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

    split_block(block, size);
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
    coalesce_free_blocks();
}

/* -------------------------------------------------------------
 * STRING UTILITIES
 * -------------------------------------------------------------
 */

void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *out = dest;
    const uint8_t *in = src;
    for (size_t i = 0; i < n; i++) {
        out[i] = in[i];
    }
    return dest;
}

void *memset(void *s, int c, size_t n)
{
    uint8_t *p = s;
    uint8_t val = (uint8_t)c;
    for (size_t i = 0; i < n; i++) {
        p[i] = val;
    }
    return s;
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char *s1, const char *s2)
{
    size_t i = 0;
    while (s1[i] != '\0' && s1[i] == s2[i]) {
        i++;
    }
    return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
}

/* -------------------------------------------------------------
 * BUFFERED PRINTF IMPLEMENTATION
 * -------------------------------------------------------------
 */

struct printf_buffer {
    char buf[PRINTF_BUF_SIZE];
    int offset;
    int written;
};

static void printf_flush(struct printf_buffer *pb)
{
    if (pb->offset > 0) {
        (void)_syscall(SYS_WRITE, 1, (long)pb->buf, pb->offset, 0, 0, 0);
        pb->written += pb->offset;
        pb->offset = 0;
    }
}

static void printf_putc(struct printf_buffer *pb, char c)
{
    if (pb->offset >= PRINTF_BUF_SIZE) {
        printf_flush(pb);
    }
    pb->buf[pb->offset++] = c;
}

static void printf_puts(struct printf_buffer *pb, const char *str)
{
    if (str == 0) {
        str = "(null)";
    }
    while (*str != '\0') {
        printf_putc(pb, *str++);
    }
}

static void printf_put_unsigned(struct printf_buffer *pb, unsigned long value, unsigned int base, int uppercase)
{
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char temp[64];
    int temp_idx = 0;

    if (value == 0) {
        printf_putc(pb, '0');
        return;
    }

    while (value != 0 && temp_idx < 64) {
        temp[temp_idx++] = digits[value % base];
        value /= base;
    }

    while (temp_idx > 0) {
        printf_putc(pb, temp[--temp_idx]);
    }
}

int printf(const char *format, ...)
{
    if (format == 0) {
        return 0;
    }

    struct printf_buffer pb;
    pb.offset = 0;
    pb.written = 0;

    va_list args;
    va_start(args, format);

    while (*format != '\0') {
        if (*format != '%') {
            printf_putc(&pb, *format++);
            continue;
        }

        format++; // skip '%'

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
            printf_putc(&pb, '%');
        } else if (spec == 'c') {
            printf_putc(&pb, (char)va_arg(args, int));
        } else if (spec == 's') {
            printf_puts(&pb, va_arg(args, const char *));
        } else if (spec == 'd' || spec == 'i') {
            long value = size_arg ? (long)va_arg(args, size_t) :
                (long_arg ? va_arg(args, long) : va_arg(args, int));
            if (value < 0) {
                printf_putc(&pb, '-');
                value = -value;
            }
            printf_put_unsigned(&pb, (unsigned long)value, 10, 0);
        } else if (spec == 'u') {
            unsigned long value = size_arg ? (unsigned long)va_arg(args, size_t) :
                (long_arg ? va_arg(args, unsigned long) : va_arg(args, unsigned int));
            printf_put_unsigned(&pb, value, 10, 0);
        } else if (spec == 'x' || spec == 'X') {
            unsigned long value = size_arg ? (unsigned long)va_arg(args, size_t) :
                (long_arg ? va_arg(args, unsigned long) : va_arg(args, unsigned int));
            printf_put_unsigned(&pb, value, 16, spec == 'X');
        } else if (spec == 'p') {
            unsigned long value = (unsigned long)va_arg(args, void *);
            printf_puts(&pb, "0x");
            printf_put_unsigned(&pb, value, 16, 0);
        } else {
            printf_putc(&pb, '%');
            printf_putc(&pb, spec);
        }
    }

    va_end(args);
    printf_flush(&pb);

    return pb.written;
}

/* -------------------------------------------------------------
 * VFS WRAPPERS AND PROCESS MANAGEMENT
 * -------------------------------------------------------------
 */

int open(const char *path, int flags)
{
    return (int)_syscall(SYS_OPEN, (long)path, (long)flags, 0, 0, 0, 0);
}

int read(int fd, void *buf, int count)
{
    return (int)_syscall(SYS_READ, (long)fd, (long)buf, (long)count, 0, 0, 0);
}

int write(int fd, const void *buf, int count)
{
    return (int)_syscall(SYS_WRITE, (long)fd, (long)buf, (long)count, 0, 0, 0);
}

int close(int fd)
{
    return (int)_syscall(SYS_CLOSE, (long)fd, 0, 0, 0, 0, 0);
}

int fork(void)
{
    return (int)_syscall(SYS_FORK, 0, 0, 0, 0, 0, 0);
}

void exit(int status)
{
    _syscall(SYS_EXIT, status, 0, 0, 0, 0, 0);
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

int socket_send(uint32_t dest_ip, uint8_t protocol, const void *payload, size_t len)
{
    return (int)syscall4(SYS_SOCKET_SEND, (long)dest_ip, (long)protocol, (long)payload, (long)len);
}

int socket_recv(uint8_t protocol, void *buffer, size_t max_len)
{
    return (int)syscall3(SYS_SOCKET_RECV, (long)protocol, (long)buffer, (long)max_len);
}

int socket(int domain, int type, int protocol)
{
    return (int)syscall3(SYS_SOCKET, domain, type, protocol);
}

int bind(int fd, const struct sockaddr *addr, uint32_t addrlen)
{
    return (int)syscall3(SYS_BIND, fd, (long)addr, addrlen);
}

int connect(int fd, const struct sockaddr *addr, uint32_t addrlen)
{
    return (int)syscall3(SYS_CONNECT, fd, (long)addr, addrlen);
}

int listen(int fd, int backlog)
{
    return (int)syscall2(SYS_LISTEN, fd, backlog);
}

int accept(int fd, struct sockaddr *addr, uint32_t *addrlen)
{
    return (int)syscall3(SYS_ACCEPT, fd, (long)addr, (long)addrlen);
}

int chmod(const char *path, uint32_t mode)
{
    return (int)syscall2(SYS_CHMOD, (long)path, (long)mode);
}

int chown(const char *path, uint32_t uid, uint32_t gid)
{
    return (int)syscall3(SYS_CHOWN, (long)path, (long)uid, (long)gid);
}

int link(const char *oldpath, const char *newpath)
{
    return (int)syscall2(SYS_LINK, (long)oldpath, (long)newpath);
}

int unlink(const char *pathname)
{
    return (int)syscall1(SYS_UNLINK, (long)pathname);
}

int symlink(const char *target, const char *linkpath)
{
    return (int)syscall2(SYS_SYMLINK, (long)target, (long)linkpath);
}

int readlink(const char *pathname, char *buf, size_t bufsiz)
{
    return (int)syscall3(SYS_READLINK, (long)pathname, (long)buf, (long)bufsiz);
}

int mount(const char *source, const char *target, const char *fs_type, uint64_t flags)
{
    return (int)syscall4(SYS_MOUNT, (long)source, (long)target, (long)fs_type, (long)flags);
}

int umount(const char *target)
{
    return (int)syscall1(SYS_UMOUNT, (long)target);
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

    return htonl((octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3]);
}

uintptr_t __stack_chk_guard = 0x123456789ABCDEF0ULL;

void __stack_chk_fail(void)
{
    exit(-1);
}
