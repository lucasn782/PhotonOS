#include "heap.h"

#include <stdint.h>

#include "memory.h"
#include "vmm.h"

#define KERNEL_HEAP_BASE 0xFFFFFFFF90000000ULL
#define KERNEL_HEAP_INITIAL_PAGES 4ULL
#define KERNEL_HEAP_MAX_PAGES 256ULL
#define HEAP_MAGIC 0x48454150424C4B31ULL
#define HEAP_MIN_SPLIT 32ULL

struct heap_block {
    uint64_t magic;
    size_t size;
    int free;
    struct heap_block *next;
    struct heap_block *prev;
};

static struct heap_block *heap_head;
static uintptr_t heap_start;
static uintptr_t heap_end;
static uintptr_t heap_limit;

static size_t align_up_size(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static uintptr_t align_up_ptr(uintptr_t value, uintptr_t alignment)
{
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static void heap_map_page(uintptr_t virtual_addr)
{
    void *physical = pmm_alloc();
    if (physical == 0) {
        return;
    }

    vmm_map(virtual_addr, (uintptr_t)physical, PAGE_PRESENT | PAGE_WRITABLE);
}

static int heap_expand(size_t required)
{
    uintptr_t old_end = heap_end;
    uintptr_t new_end = align_up_ptr(heap_end + required, PMM_PAGE_SIZE);

    if (new_end > heap_limit) {
        return 0;
    }

    for (uintptr_t page = heap_end; page < new_end; page += PMM_PAGE_SIZE) {
        heap_map_page(page);
    }

    heap_end = new_end;
    size_t added = heap_end - old_end;

    struct heap_block *tail = heap_head;
    while (tail != 0 && tail->next != 0) {
        tail = tail->next;
    }

    if (tail != 0 && tail->free) {
        tail->size += added;
        return 1;
    }

    struct heap_block *block = (struct heap_block *)old_end;
    block->magic = HEAP_MAGIC;
    block->size = added - sizeof(*block);
    block->free = 1;
    block->next = 0;
    block->prev = tail;

    if (tail != 0) {
        tail->next = block;
    } else {
        heap_head = block;
    }

    return 1;
}

static void heap_split_block(struct heap_block *block, size_t size)
{
    if (block->size < size + sizeof(*block) + HEAP_MIN_SPLIT) {
        return;
    }

    struct heap_block *new_block =
        (struct heap_block *)((uint8_t *)(block + 1) + size);

    new_block->magic = HEAP_MAGIC;
    new_block->size = block->size - size - sizeof(*block);
    new_block->free = 1;
    new_block->next = block->next;
    new_block->prev = block;

    if (new_block->next != 0) {
        new_block->next->prev = new_block;
    }

    block->size = size;
    block->next = new_block;
}

static void heap_coalesce(struct heap_block *block)
{
    if (block->next != 0 && block->next->free) {
        struct heap_block *next = block->next;
        block->size += sizeof(*next) + next->size;
        block->next = next->next;
        if (block->next != 0) {
            block->next->prev = block;
        }
    }

    if (block->prev != 0 && block->prev->free) {
        heap_coalesce(block->prev);
    }
}

void heap_init(void)
{
    heap_start = KERNEL_HEAP_BASE;
    heap_end = KERNEL_HEAP_BASE;
    heap_limit = KERNEL_HEAP_BASE + (KERNEL_HEAP_MAX_PAGES * PMM_PAGE_SIZE);
    heap_head = 0;

    heap_expand(KERNEL_HEAP_INITIAL_PAGES * PMM_PAGE_SIZE);
}

void *kmalloc(size_t size)
{
    if (size == 0) {
        return 0;
    }

    size = align_up_size(size, 16);

    for (;;) {
        for (struct heap_block *block = heap_head; block != 0; block = block->next) {
            if (block->magic == HEAP_MAGIC && block->free && block->size >= size) {
                heap_split_block(block, size);
                block->free = 0;
                return block + 1;
            }
        }

        if (!heap_expand(size + sizeof(struct heap_block))) {
            return 0;
        }
    }
}

void kfree(void *ptr)
{
    if (ptr == 0) {
        return;
    }

    struct heap_block *block = ((struct heap_block *)ptr) - 1;
    if (block->magic != HEAP_MAGIC) {
        return;
    }

    block->free = 1;
    heap_coalesce(block);
}
