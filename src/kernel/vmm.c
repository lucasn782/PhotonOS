#include "vmm.h"

#include <stddef.h>

#include "memory.h"

#define VMM_PAGE_SIZE 4096ULL
#define VMM_PAGE_MASK (~(VMM_PAGE_SIZE - 1ULL))
#define VMM_ENTRY_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define VMM_ENTRY_FLAGS_MASK 0xFFFULL
#define VMM_TABLE_ENTRIES 512

static uint64_t *kernel_pml4;

static uint64_t read_cr3(void)
{
    uint64_t value;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(value));
    return value;
}

static void clear_page(void *page)
{
    uint64_t *entries = page;

    for (uint64_t i = 0; i < VMM_TABLE_ENTRIES; i++) {
        entries[i] = 0;
    }
}

static void copy_page(void *dest, const void *src)
{
    uint64_t *dest_entries = dest;
    const uint64_t *src_entries = src;

    for (uint64_t i = 0; i < VMM_TABLE_ENTRIES; i++) {
        dest_entries[i] = src_entries[i];
    }
}

static uint64_t table_flags(uint32_t flags)
{
    return VMM_PRESENT | VMM_WRITABLE | (flags & VMM_USER);
}

static uint64_t *entry_table(uint64_t entry)
{
    return (uint64_t *)(entry & VMM_ENTRY_ADDR_MASK);
}

static int valid_table_entry(uint64_t entry)
{
    uint64_t address = entry & VMM_ENTRY_ADDR_MASK;

    return address != 0 && address < PMM_TOTAL_MEMORY &&
        (address & (VMM_PAGE_SIZE - 1ULL)) == 0;
}

static int valid_table_pointer(uint64_t *table)
{
    uint64_t address = (uint64_t)table;

    return address != 0 && address < PMM_TOTAL_MEMORY &&
        (address & (VMM_PAGE_SIZE - 1ULL)) == 0;
}

static uint64_t *ensure_next_table(uint64_t *table, uint64_t index, uint32_t flags)
{
    if (!valid_table_pointer(table) || index >= VMM_TABLE_ENTRIES) {
        return NULL;
    }

    if ((table[index] & VMM_PAGE_PRESENT) != 0 &&
        !valid_table_entry(table[index])) {
        table[index] = 0;
    }

    if ((table[index] & VMM_PAGE_PRESENT) == 0) {
        void *new_table = pmm_alloc();
        if (new_table == NULL) {
            return NULL;
        }

        clear_page(new_table);
        table[index] = ((uint64_t)new_table & VMM_ENTRY_ADDR_MASK) |
            table_flags(flags);
    } else if ((flags & VMM_USER) && (table[index] & VMM_USER) == 0) {
        void *new_table = pmm_alloc();
        if (new_table == NULL) {
            return NULL;
        }

        copy_page(new_table, entry_table(table[index]));
        table[index] = ((uint64_t)new_table & VMM_ENTRY_ADDR_MASK) |
            table_flags(flags);
    }

    uint64_t *next = entry_table(table[index]);
    if (!valid_table_pointer(next)) {
        void *new_table = pmm_alloc();
        if (new_table == NULL) {
            return NULL;
        }

        clear_page(new_table);
        table[index] = ((uint64_t)new_table & VMM_ENTRY_ADDR_MASK) |
            table_flags(flags);
        return new_table;
    }

    return next;
}

static void map_in_pml4(uint64_t *pml4, uintptr_t virtual_addr,
    uintptr_t physical_addr, uint32_t flags)
{
    uintptr_t vaddr = virtual_addr & VMM_PAGE_MASK;
    uintptr_t paddr = physical_addr & VMM_PAGE_MASK;
    uintptr_t pml4_index = (vaddr >> 39) & 0x1FFULL;
    uintptr_t pdpt_index = (vaddr >> 30) & 0x1FFULL;
    uintptr_t pd_index = (vaddr >> 21) & 0x1FFULL;
    uintptr_t pt_index = (vaddr >> 12) & 0x1FFULL;

    uint64_t *pdpt = ensure_next_table(pml4, pml4_index, flags);
    if (pdpt == NULL) {
        return;
    }

    uint64_t *pd = ensure_next_table(pdpt, pdpt_index, flags);
    if (pd == NULL) {
        return;
    }

    uint64_t *pt = ensure_next_table(pd, pd_index, flags);
    if (pt == NULL) {
        return;
    }

    pt[pt_index] = (paddr & VMM_ENTRY_ADDR_MASK) |
        (flags & VMM_ENTRY_FLAGS_MASK);
}

void vmm_init(void)
{
    kernel_pml4 = (uint64_t *)(read_cr3() & VMM_ENTRY_ADDR_MASK);
}

void vmm_flush(uintptr_t addr)
{
    vmm_flush_tlb(addr);
}

void vmm_map(uintptr_t virtual_addr, uintptr_t physical_addr, uint32_t flags)
{
    if (kernel_pml4 == NULL) {
        vmm_init();
    }

    map_in_pml4(kernel_pml4, virtual_addr, physical_addr, flags);
    vmm_flush(virtual_addr & VMM_PAGE_MASK);
}

uintptr_t vmm_virt_to_phys(uintptr_t virtual_addr)
{
    uint64_t *pml4 = vmm_kernel_pml4();
    uintptr_t vaddr = virtual_addr & VMM_PAGE_MASK;
    uintptr_t pml4_index = (vaddr >> 39) & 0x1FFULL;
    uintptr_t pdpt_index = (vaddr >> 30) & 0x1FFULL;
    uintptr_t pd_index = (vaddr >> 21) & 0x1FFULL;
    uintptr_t pt_index = (vaddr >> 12) & 0x1FFULL;

    if (!valid_table_pointer(pml4) ||
        (pml4[pml4_index] & VMM_PAGE_PRESENT) == 0 ||
        !valid_table_entry(pml4[pml4_index])) {
        return 0;
    }

    uint64_t *pdpt = entry_table(pml4[pml4_index]);
    if ((pdpt[pdpt_index] & VMM_PAGE_PRESENT) == 0 ||
        !valid_table_entry(pdpt[pdpt_index])) {
        return 0;
    }

    uint64_t *pd = entry_table(pdpt[pdpt_index]);
    if ((pd[pd_index] & VMM_PAGE_PRESENT) == 0 ||
        !valid_table_entry(pd[pd_index])) {
        return 0;
    }

    uint64_t *pt = entry_table(pd[pd_index]);
    if (!valid_table_pointer(pt) ||
        (pt[pt_index] & VMM_PAGE_PRESENT) == 0) {
        return 0;
    }

    return (uintptr_t)(pt[pt_index] & VMM_ENTRY_ADDR_MASK) |
        (virtual_addr & (VMM_PAGE_SIZE - 1ULL));
}

uint64_t *vmm_kernel_pml4(void)
{
    if (kernel_pml4 == NULL) {
        vmm_init();
    }

    return kernel_pml4;
}

uint64_t *vmm_create_address_space(void)
{
    uint64_t *pml4 = pmm_alloc();
    if (pml4 == NULL) {
        return NULL;
    }

    clear_page(pml4);

    uint64_t *kernel = vmm_kernel_pml4();

    /* Clone the low identity mapping (covers e1000 MMIO at 0xF0000000 and
     * other lower-half kernel structures used during early boot). */
    pml4[0] = kernel[0];

    /* Clone the entire upper half (entries 256-511) so that kernel heap,
     * MMIO windows, APIC, and all higher-half kernel mappings remain
     * accessible while the CPU is running with a user-process CR3. */
    for (int i = 256; i < 512; i++) {
        pml4[i] = kernel[i];
    }

    return pml4;
}

void vmm_destroy_address_space(uint64_t *pml4)
{
    if (pml4 == NULL || pml4 == vmm_kernel_pml4()) {
        return;
    }

    for (uint64_t pml4_i = 0; pml4_i < VMM_TABLE_ENTRIES; pml4_i++) {
        if ((pml4[pml4_i] & VMM_PAGE_PRESENT) == 0 ||
            (pml4[pml4_i] & VMM_USER) == 0) {
            continue;
        }

        uint64_t *pdpt = entry_table(pml4[pml4_i]);
        for (uint64_t pdpt_i = 0; pdpt_i < VMM_TABLE_ENTRIES; pdpt_i++) {
            if ((pdpt[pdpt_i] & VMM_PAGE_PRESENT) == 0 ||
                (pdpt[pdpt_i] & VMM_USER) == 0) {
                continue;
            }

            uint64_t *pd = entry_table(pdpt[pdpt_i]);
            for (uint64_t pd_i = 0; pd_i < VMM_TABLE_ENTRIES; pd_i++) {
                if ((pd[pd_i] & VMM_PAGE_PRESENT) == 0 ||
                    (pd[pd_i] & VMM_USER) == 0) {
                    continue;
                }

                pmm_free(entry_table(pd[pd_i]));
            }

            pmm_free(pd);
        }

        pmm_free(pdpt);
    }

    pmm_free(pml4);
}

void vmm_map_in_space(uint64_t *pml4, uintptr_t virtual_addr,
    uintptr_t physical_addr, uint32_t flags)
{
    if (pml4 == NULL) {
        return;
    }

    map_in_pml4(pml4, virtual_addr, physical_addr, flags);
}

int vmm_is_mapped(uint64_t *pml4, uintptr_t virtual_addr)
{
    if (!valid_table_pointer(pml4)) {
        return 0;
    }

    uintptr_t vaddr = virtual_addr & VMM_PAGE_MASK;
    uintptr_t pml4_index = (vaddr >> 39) & 0x1FFULL;
    uintptr_t pdpt_index = (vaddr >> 30) & 0x1FFULL;
    uintptr_t pd_index = (vaddr >> 21) & 0x1FFULL;
    uintptr_t pt_index = (vaddr >> 12) & 0x1FFULL;

    if ((pml4[pml4_index] & VMM_PAGE_PRESENT) == 0 ||
        !valid_table_entry(pml4[pml4_index])) {
        return 0;
    }

    uint64_t *pdpt = entry_table(pml4[pml4_index]);
    if ((pdpt[pdpt_index] & VMM_PAGE_PRESENT) == 0 ||
        !valid_table_entry(pdpt[pdpt_index])) {
        return 0;
    }

    uint64_t *pd = entry_table(pdpt[pdpt_index]);
    if ((pd[pd_index] & VMM_PAGE_PRESENT) == 0 ||
        !valid_table_entry(pd[pd_index])) {
        return 0;
    }

    uint64_t *pt = entry_table(pd[pd_index]);
    if (!valid_table_pointer(pt)) {
        return 0;
    }

    return (pt[pt_index] & VMM_PAGE_PRESENT) != 0;
}

void vmm_switch_address_space(uint64_t *pml4)
{
    if (pml4 == NULL) {
        return;
    }

    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)pml4) : "memory");
}

/*
 * vmm_clone_address_space - Clona o espaco de endereçamento do processo pai
 * para um novo PML4 de filho.
 *
 * Higher-half (entradas 256-511): copiadas por referencia (Kernel compartilhado).
 * Entrada 0 (identity map de boot): copiada por referencia.
 * Lower-half de usuario (entradas 1-255 com bit USER): deep-copy por pagina.
 *   - Aloca novo frame fisico via pmm_alloc().
 *   - Copia 4096 bytes do frame do pai para o frame do filho.
 *   - Mapeia o novo frame na PML4 do filho com as mesmas flags.
 *
 * Retorna ponteiro para a nova PML4 ou NULL em caso de falha.
 */
uint64_t *vmm_clone_address_space(uint64_t *parent_pml4)
{
    if (!valid_table_pointer(parent_pml4)) {
        return NULL;
    }

    uint64_t *child_pml4 = pmm_alloc();
    if (child_pml4 == NULL) {
        return NULL;
    }
    clear_page(child_pml4);

    /* Entrada 0: identity-map de boot (MMIO, early structures). */
    child_pml4[0] = parent_pml4[0];

    /* Higher-half (256-511): mapeamentos do Kernel - compartilhamento direto. */
    for (int i = 256; i < 512; i++) {
        child_pml4[i] = parent_pml4[i];
    }

    /* Lower-half de usuario (1-255): deep-copy por pagina. */
    for (uintptr_t pml4_i = 1; pml4_i < 256; pml4_i++) {
        if ((parent_pml4[pml4_i] & VMM_PAGE_PRESENT) == 0 ||
            (parent_pml4[pml4_i] & VMM_USER) == 0 ||
            !valid_table_entry(parent_pml4[pml4_i])) {
            continue;
        }

        uint64_t *parent_pdpt = entry_table(parent_pml4[pml4_i]);
        if (!valid_table_pointer(parent_pdpt)) {
            continue;
        }

        for (uintptr_t pdpt_i = 0; pdpt_i < VMM_TABLE_ENTRIES; pdpt_i++) {
            if ((parent_pdpt[pdpt_i] & VMM_PAGE_PRESENT) == 0 ||
                (parent_pdpt[pdpt_i] & VMM_USER) == 0 ||
                !valid_table_entry(parent_pdpt[pdpt_i])) {
                continue;
            }

            uint64_t *parent_pd = entry_table(parent_pdpt[pdpt_i]);
            if (!valid_table_pointer(parent_pd)) {
                continue;
            }

            for (uintptr_t pd_i = 0; pd_i < VMM_TABLE_ENTRIES; pd_i++) {
                if ((parent_pd[pd_i] & VMM_PAGE_PRESENT) == 0 ||
                    (parent_pd[pd_i] & VMM_USER) == 0 ||
                    !valid_table_entry(parent_pd[pd_i])) {
                    continue;
                }

                uint64_t *parent_pt = entry_table(parent_pd[pd_i]);
                if (!valid_table_pointer(parent_pt)) {
                    continue;
                }

                for (uintptr_t pt_i = 0; pt_i < VMM_TABLE_ENTRIES; pt_i++) {
                    if ((parent_pt[pt_i] & VMM_PAGE_PRESENT) == 0) {
                        continue;
                    }

                    /* Calcula o endereco virtual desta pagina. */
                    uintptr_t virt = (pml4_i << 39) | (pdpt_i << 30) |
                                     (pd_i   << 21) | (pt_i   << 12);

                    /* Aloca novo frame fisico para o filho. */
                    void *child_frame = pmm_alloc();
                    if (child_frame == NULL) {
                        /* Falha de memoria: retorna NULL sem destruir parcialmente. */
                        return NULL;
                    }

                    /* Copia profunda: 4096 bytes do frame do pai -> frame do filho. */
                    uint64_t parent_phys = parent_pt[pt_i] & VMM_ENTRY_ADDR_MASK;
                    uint8_t *src = (uint8_t *)parent_phys;
                    uint8_t *dst = (uint8_t *)child_frame;
                    for (uint64_t b = 0; b < VMM_PAGE_SIZE; b++) {
                        dst[b] = src[b];
                    }

                    /* Preserva as mesmas flags de permissao do pai. */
                    uint32_t flags = (uint32_t)(parent_pt[pt_i] & VMM_ENTRY_FLAGS_MASK);
                    map_in_pml4(child_pml4, virt, (uintptr_t)child_frame, flags);
                }
            }
        }
    }

    return child_pml4;
}
