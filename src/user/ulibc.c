#include "ulibc.h"

#include <stdint.h>

#define SYS_BRK 9
#define SYS_SIGNAL 10
#define SYS_KILL 11
#define SYS_SIGRETURN 12
#define SYS_GETPROCS 13
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
