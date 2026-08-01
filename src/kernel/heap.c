#include "heap.h"

#include <stdint.h>

#include "memory.h"
#include "vmm.h"
#include "smp.h"

static spinlock_t heap_lock;


#define KERNEL_HEAP_BASE 0xFFFFFFFF90000000ULL
#define KERNEL_HEAP_INITIAL_PAGES 4ULL
#define KERNEL_HEAP_MAX_PAGES 256ULL
#define HEAP_MAGIC 0x48454150424C4B31ULL
#define HEAP_FREED_MAGIC 0xDEADBEEF48454150ULL
#define HEAP_POISON_BYTE 0xDD
#define HEAP_MIN_SPLIT 32ULL

extern void klog(const char *fmt, ...);

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

static int heap_map_page(uintptr_t virtual_addr)
{
    void *physical = pmm_alloc();
    if (physical == 0) {
        klog("HEAP MAP FAIL\n");
        return -1;
    }

    vmm_map(virtual_addr, (uintptr_t)physical, PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX);
    if (!vmm_is_mapped(vmm_kernel_pml4(), virtual_addr)) {
        klog("HEAP MAP FAIL\n");
        pmm_free(physical);
        return -1;
    }

    klog("HEAP MAP OK\n");
    return 0;
}

static int heap_expand(size_t required)
{
    uintptr_t old_end = heap_end;
    uintptr_t new_end = align_up_ptr(heap_end + required, PMM_PAGE_SIZE);

    if (new_end > heap_limit) {
        klog("HEAP EXPAND FAIL\n");
        return 0;
    }

    for (uintptr_t page = heap_end; page < new_end; page += PMM_PAGE_SIZE) {
        if (heap_map_page(page) != 0) {
            klog("HEAP EXPAND FAIL\n");
            return 0;
        }
    }

    heap_end = new_end;
    size_t added = heap_end - old_end;

    struct heap_block *tail = heap_head;
    while (tail != 0 && tail->next != 0) {
        tail = tail->next;
    }

    if (tail != 0 && tail->free) {
        tail->size += added;
        klog("HEAP EXPAND OK\n");
        return 1;
    }

    struct heap_block *block = (struct heap_block *)old_end;
    block->magic = HEAP_FREED_MAGIC;
    block->size = added - sizeof(*block);
    block->free = 1;
    block->next = 0;
    block->prev = tail;

    if (tail != 0) {
        tail->next = block;
    } else {
        heap_head = block;
    }

    klog("HEAP EXPAND OK\n");
    return 1;
}

static void heap_split_block(struct heap_block *block, size_t size)
{
    if (block->size < size + sizeof(*block) + HEAP_MIN_SPLIT) {
        return;
    }

    struct heap_block *new_block =
        (struct heap_block *)((uint8_t *)(block + 1) + size);

    new_block->magic = HEAP_FREED_MAGIC;
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

int heap_validate(void)
{
    uint64_t flags = spin_lock_irqsave(&heap_lock);

    struct heap_block *curr = heap_head;
    while (curr != 0) {
        if (curr->magic != HEAP_MAGIC && curr->magic != HEAP_FREED_MAGIC) {
            klog("heap: corrupt magic at block %p\n", curr);
            spin_unlock_irqrestore(&heap_lock, flags);
            return 0;
        }

        if (curr->next != 0 && curr->next->prev != curr) {
            klog("heap: broken link at block %p\n", curr);
            spin_unlock_irqrestore(&heap_lock, flags);
            return 0;
        }

        curr = curr->next;
    }

    spin_unlock_irqrestore(&heap_lock, flags);
    return 1;
}

void heap_init(void)
{
    spin_init(&heap_lock);
    heap_start = KERNEL_HEAP_BASE;
    heap_end = KERNEL_HEAP_BASE;
    heap_limit = KERNEL_HEAP_BASE + (KERNEL_HEAP_MAX_PAGES * PMM_PAGE_SIZE);
    heap_head = 0;

    heap_expand(KERNEL_HEAP_INITIAL_PAGES * PMM_PAGE_SIZE);
    klog("heap: inicializado com W^X (PAGE_NX), verificador e deteccao de Double-Free/UAF.\n");
}

void *kmalloc(size_t size)
{
    if (size == 0) {
        return 0;
    }

    size = align_up_size(size, 16);

    uint64_t flags = spin_lock_irqsave(&heap_lock);

    for (;;) {
        for (struct heap_block *block = heap_head; block != 0; block = block->next) {
            if ((block->magic == HEAP_MAGIC || block->magic == HEAP_FREED_MAGIC) && block->free && block->size >= size) {
                heap_split_block(block, size);
                block->magic = HEAP_MAGIC;
                block->free = 0;
                spin_unlock_irqrestore(&heap_lock, flags);
                return block + 1;
            }
        }

        if (!heap_expand(size + sizeof(struct heap_block))) {
            spin_unlock_irqrestore(&heap_lock, flags);
            return 0;
        }
    }
}

void kfree(void *ptr)
{
    if (ptr == 0) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&heap_lock);

    struct heap_block *block = ((struct heap_block *)ptr) - 1;
    uintptr_t block_addr = (uintptr_t)block;

    if (block_addr < heap_start || block_addr >= heap_end) {
        klog("heap: kfree address %p out of bounds\n", ptr);
        spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }

    if (block->magic == HEAP_FREED_MAGIC || block->free) {
        klog("heap: DOUBLE FREE DETECTED at %p!\n", ptr);
        spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }

    if (block->magic != HEAP_MAGIC) {
        klog("heap: CORRUPTION DETECTED at block %p\n", block);
        spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }

    /* Use-After-Free Poisoning */
    uint8_t *payload = (uint8_t *)(block + 1);
    for (size_t i = 0; i < block->size; i++) {
        payload[i] = HEAP_POISON_BYTE;
    }

    block->magic = HEAP_FREED_MAGIC;
    block->free = 1;
    heap_coalesce(block);

    spin_unlock_irqrestore(&heap_lock, flags);
}
