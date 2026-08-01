#include "memory.h"

#include <stddef.h>

#define PMM_MAX_BLOCKS (PMM_TOTAL_MEMORY / PMM_PAGE_SIZE)
#define PMM_BITMAP_WORDS ((PMM_MAX_BLOCKS + 63ULL) / 64ULL)

extern uint8_t __kernel_end[];

static uint64_t pmm_bitmap[PMM_BITMAP_WORDS];
static uint32_t pmm_refcounts[PMM_MAX_BLOCKS];
static uint64_t pmm_total_block_count;
static uint64_t pmm_reserved_block_count;

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static void bitmap_set(uint64_t bit)
{
    pmm_bitmap[bit / 64ULL] |= 1ULL << (bit % 64ULL);
}

static void bitmap_clear(uint64_t bit)
{
    pmm_bitmap[bit / 64ULL] &= ~(1ULL << (bit % 64ULL));
}

extern void klog(char *msg);

static int bitmap_test(uint64_t bit)
{
    return (pmm_bitmap[bit / 64ULL] & (1ULL << (bit % 64ULL))) != 0;
}

void pmm_init(void)
{
    pmm_total_block_count = PMM_MAX_BLOCKS;

    for (uint64_t i = 0; i < PMM_BITMAP_WORDS; i++) {
        pmm_bitmap[i] = 0;
    }
    for (uint64_t i = 0; i < PMM_MAX_BLOCKS; i++) {
        pmm_refcounts[i] = 0;
    }

    uint64_t reserved_end = align_up(PMM_RESERVED_END, PMM_PAGE_SIZE);
    uint64_t kernel_end = align_up((uint64_t)__kernel_end, PMM_PAGE_SIZE);
    if (kernel_end > reserved_end) {
        reserved_end = kernel_end;
    }

    pmm_reserved_block_count = reserved_end / PMM_PAGE_SIZE;
    if (pmm_reserved_block_count > pmm_total_block_count) {
        pmm_reserved_block_count = pmm_total_block_count;
    }

    for (uint64_t i = 0; i < pmm_reserved_block_count; i++) {
        bitmap_set(i);
        pmm_refcounts[i] = 1;
    }

    klog("PMM RESERVED\n");
    klog("PMM FREE\n");
}

void *pmm_alloc(void)
{
    for (uint64_t i = 0; i < pmm_total_block_count; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            pmm_refcounts[i] = 1;
            return (void *)(i * PMM_PAGE_SIZE);
        }
    }

    klog("PMM LAST FRAME NULL\n");
    return NULL;
}

void pmm_free(void *ptr)
{
    uint64_t address = (uint64_t)ptr;
    if ((address % PMM_PAGE_SIZE) != 0) {
        return;
    }

    uint64_t block = address / PMM_PAGE_SIZE;
    if (block >= pmm_total_block_count || block < pmm_reserved_block_count) {
        return;
    }

    if (pmm_refcounts[block] > 0) {
        pmm_refcounts[block]--;
        if (pmm_refcounts[block] == 0) {
            bitmap_clear(block);
        }
    }
}

void pmm_ref_inc(void *ptr)
{
    uint64_t address = (uint64_t)ptr;
    if ((address % PMM_PAGE_SIZE) != 0) {
        return;
    }

    uint64_t block = address / PMM_PAGE_SIZE;
    if (block >= pmm_total_block_count) {
        return;
    }

    pmm_refcounts[block]++;
}

uint32_t pmm_ref_get(void *ptr)
{
    uint64_t address = (uint64_t)ptr;
    if ((address % PMM_PAGE_SIZE) != 0) {
        return 0;
    }

    uint64_t block = address / PMM_PAGE_SIZE;
    if (block >= pmm_total_block_count) {
        return 0;
    }

    return pmm_refcounts[block];
}

uint64_t pmm_total_blocks(void)
{
    return pmm_total_block_count;
}

uint64_t pmm_reserved_blocks(void)
{
    return pmm_reserved_block_count;
}

uint64_t pmm_used_blocks(void)
{
    uint64_t used = 0;

    for (uint64_t i = 0; i < pmm_total_block_count; i++) {
        if (bitmap_test(i)) {
            used++;
        }
    }

    return used;
}

uint64_t pmm_free_blocks(void)
{
    return pmm_total_block_count - pmm_used_blocks();
}

uint64_t pmm_total_memory(void)
{
    return pmm_total_block_count * PMM_PAGE_SIZE;
}

uint64_t pmm_free_memory(void)
{
    return pmm_free_blocks() * PMM_PAGE_SIZE;
}

uint64_t pmm_free_memory_mib(void)
{
    return pmm_free_memory() / (1024ULL * 1024ULL);
}
