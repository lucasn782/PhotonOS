#ifndef PHOTONOS_VMM_H
#define PHOTONOS_VMM_H

#include <stdint.h>

#define PAGE_PRESENT  (1U << 0)
#define PAGE_WRITABLE (1U << 1)
#define PAGE_USER     (1U << 2)
#define PAGE_WRITE_THROUGH (1U << 3)
#define PAGE_CACHE_DISABLE (1U << 4)
#define PAGE_COW      (1ULL << 9)

#define VMM_PRESENT  (1U << 0)
#define VMM_WRITABLE (1U << 1)
#define VMM_USER     (1U << 2)
#define VMM_WRITE_THROUGH (1U << 3)
#define VMM_CACHE_DISABLE (1U << 4)
#define VMM_COW      (1ULL << 9)

#define VMM_PAGE_PRESENT VMM_PRESENT
#define VMM_PAGE_WRITE   VMM_WRITABLE
#define VMM_PAGE_USER    VMM_USER
#define VMM_PAGE_WRITE_THROUGH VMM_WRITE_THROUGH
#define VMM_PAGE_CACHE_DISABLE VMM_CACHE_DISABLE

void vmm_init(void);
void vmm_flush(uintptr_t addr);
void vmm_map(uintptr_t virtual_addr, uintptr_t physical_addr, uint32_t flags);
uintptr_t vmm_virt_to_phys(uintptr_t virtual_addr);
uint64_t *vmm_kernel_pml4(void);
uint64_t *vmm_create_address_space(void);
void vmm_destroy_address_space(uint64_t *pml4);
void vmm_map_in_space(uint64_t *pml4, uintptr_t virtual_addr,
    uintptr_t physical_addr, uint32_t flags);
int vmm_is_mapped(uint64_t *pml4, uintptr_t virtual_addr);
void vmm_switch_address_space(uint64_t *pml4);
uint64_t *vmm_clone_address_space(uint64_t *parent_pml4);
void vmm_page_fault_handler(uint64_t error_code, uintptr_t fault_addr, uintptr_t rip);

static inline void vmm_flush_tlb(uintptr_t addr)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

#endif
