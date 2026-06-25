#ifndef PHOTONOS_MEMORY_H
#define PHOTONOS_MEMORY_H

#include <stdint.h>

#define PMM_PAGE_SIZE 4096ULL
#define PMM_TOTAL_MEMORY (128ULL * 1024ULL * 1024ULL)
#define PMM_RESERVED_END 0x100000ULL

void pmm_init(void);
void *pmm_alloc(void);
void pmm_free(void *ptr);
void pmm_ref_inc(void *ptr);
uint32_t pmm_ref_get(void *ptr);

uint64_t pmm_total_blocks(void);
uint64_t pmm_reserved_blocks(void);
uint64_t pmm_used_blocks(void);
uint64_t pmm_free_blocks(void);
uint64_t pmm_total_memory(void);
uint64_t pmm_free_memory(void);
uint64_t pmm_free_memory_mib(void);

#endif
