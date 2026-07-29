#include "elf.h"

#include <stddef.h>

#include "memory.h"
#include "scheduler.h"
#include "vfs.h"
#include "vmm.h"

#define ELF_USER_STACK_TOP 0x0000008000010000ULL
#define ELF_USER_STACK_PAGES 4ULL
#define PAGE_MASK (~(PMM_PAGE_SIZE - 1ULL))

static const uint8_t signal_trampoline_code[] = {
    0x48, 0xC7, 0xC0, 0x0C, 0x00, 0x00, 0x00,
    0x0F, 0x05,
    0xF3, 0x90,
    0xEB, 0xFC,
};

static uint64_t align_down(uint64_t value)
{
    return value & PAGE_MASK;
}

static uint64_t align_up(uint64_t value)
{
    return (value + PMM_PAGE_SIZE - 1ULL) & PAGE_MASK;
}

static void memory_zero(void *ptr, size_t size)
{
    uint8_t *bytes = ptr;

    for (size_t i = 0; i < size; i++) {
        bytes[i] = 0;
    }
}

static const char *path_basename(const char *path)
{
    const char *name = path;

    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            name = path + i + 1;
        }
    }

    return name;
}

static void copy_process_name(task_t *task, const char *path)
{
    const char *name = path_basename(path);
    size_t i = 0;
    int has_dot = 0;

    while (name[i] != '\0' && i < TASK_NAME_MAX - 1U) {
        if (name[i] == '.') {
            has_dot = 1;
        }
        task->name[i] = name[i];
        i++;
    }

    if (!has_dot && i + 4U < TASK_NAME_MAX) {
        task->name[i++] = '.';
        task->name[i++] = 'e';
        task->name[i++] = 'l';
        task->name[i++] = 'f';
    }

    task->name[i] = '\0';
}

static int elf_validate(const Elf64_Ehdr *ehdr)
{
    return ehdr->e_ident[0] == ELFMAG0 &&
        ehdr->e_ident[1] == ELFMAG1 &&
        ehdr->e_ident[2] == ELFMAG2 &&
        ehdr->e_ident[3] == ELFMAG3 &&
        ehdr->e_ident[4] == ELFCLASS64 &&
        ehdr->e_ident[5] == ELFDATA2LSB &&
        ehdr->e_type == ET_EXEC &&
        ehdr->e_machine == EM_X86_64 &&
        ehdr->e_phentsize == sizeof(Elf64_Phdr);
}

static uint64_t segment_page_flags(const Elf64_Phdr *phdr)
{
    uint64_t flags = PAGE_PRESENT | PAGE_USER;

    if (phdr->p_flags & PF_W) {
        flags |= PAGE_WRITABLE;
    }

    return flags;
}

static int track_user_page(task_t *task, void *physical)
{
    if (task->user_page_count >= TASK_MAX_USER_PAGES) {
        return 0;
    }

    task->user_physical_pages[task->user_page_count++] = (uint64_t)physical;
    return 1;
}

static int map_segment(vfs_node_t *node, uint64_t *pml4, const Elf64_Phdr *phdr,
    task_t *tracking_task)
{
    uint64_t segment_start = align_down(phdr->p_vaddr);
    uint64_t segment_end = align_up(phdr->p_vaddr + phdr->p_memsz);
    uint64_t flags = segment_page_flags(phdr);

    for (uint64_t page = segment_start; page < segment_end; page += PMM_PAGE_SIZE) {
        uint8_t *physical = pmm_alloc();
        if (physical == 0) {
            return 0;
        }

        if (!track_user_page(tracking_task, physical)) {
            pmm_free(physical);
            return 0;
        }

        memory_zero(physical, PMM_PAGE_SIZE);

        uint64_t page_file_start = 0;
        uint64_t page_file_end = 0;
        uint64_t segment_file_start = phdr->p_vaddr;
        uint64_t segment_file_end = phdr->p_vaddr + phdr->p_filesz;

        if (page + PMM_PAGE_SIZE > segment_file_start &&
            page < segment_file_end) {
            page_file_start = page > segment_file_start ?
                page : segment_file_start;
            page_file_end = page + PMM_PAGE_SIZE < segment_file_end ?
                page + PMM_PAGE_SIZE : segment_file_end;

            vfs_read(node, phdr->p_offset + (page_file_start - phdr->p_vaddr),
                page_file_end - page_file_start,
                physical + (page_file_start - page));
        }

        vmm_map_in_space(pml4, page, (uintptr_t)physical, flags);
    }

    return 1;
}

static int map_user_stack(uint64_t *pml4, task_t *tracking_task, const char *arg_str)
{
    uint64_t stack_bottom =
        ELF_USER_STACK_TOP - (ELF_USER_STACK_PAGES * PMM_PAGE_SIZE);
    void *top_stack_page_phys = NULL;

    for (uint64_t page = stack_bottom; page < ELF_USER_STACK_TOP;
        page += PMM_PAGE_SIZE) {
        void *physical = pmm_alloc();
        if (physical == 0) {
            return 0;
        }

        if (!track_user_page(tracking_task, physical)) {
            pmm_free(physical);
            return 0;
        }

        memory_zero(physical, PMM_PAGE_SIZE);
        vmm_map_in_space(pml4, page, (uintptr_t)physical,
            PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);

        top_stack_page_phys = physical;
    }

    if (arg_str != 0 && top_stack_page_phys != NULL) {
        char *dest = (char *)top_stack_page_phys + PMM_PAGE_SIZE - 256;
        size_t idx = 0;
        while (arg_str[idx] != '\0' && idx < 255) {
            dest[idx] = arg_str[idx];
            idx++;
        }
        dest[idx] = '\0';
    }

    return 1;
}

static int map_signal_trampoline(uint64_t *pml4, task_t *tracking_task)
{
    uint8_t *physical = pmm_alloc();
    if (physical == 0) {
        return 0;
    }

    if (!track_user_page(tracking_task, physical)) {
        pmm_free(physical);
        return 0;
    }

    memory_zero(physical, PMM_PAGE_SIZE);
    for (size_t i = 0; i < sizeof(signal_trampoline_code); i++) {
        physical[i] = signal_trampoline_code[i];
    }

    vmm_map_in_space(pml4, SIGNAL_TRAMPOLINE_ADDR, (uintptr_t)physical,
        PAGE_PRESENT | PAGE_USER);
    return 1;
}

int elf_load_process(const char *path, task_t *out_task)
{
    char file_path[256];
    char arg_str[256];
    int has_arg = 0;

    // Parse path to separate filename and arguments
    size_t i = 0;
    while (path[i] != '\0' && path[i] != ' ' && i < sizeof(file_path) - 1) {
        file_path[i] = path[i];
        i++;
    }
    file_path[i] = '\0';

    // Skip spaces
    while (path[i] == ' ') {
        i++;
    }

    if (path[i] != '\0') {
        has_arg = 1;
        size_t j = 0;
        while (path[i] != '\0' && j < sizeof(arg_str) - 1) {
            arg_str[j++] = path[i++];
        }
        arg_str[j] = '\0';
    }

    vfs_node_t *node = vfs_find(file_path);
    if (node == 0 || node->type != VFS_NODE_FILE) {
        return -1;
    }

    Elf64_Ehdr ehdr;
    if (vfs_read(node, 0, sizeof(ehdr), (uint8_t *)&ehdr) != sizeof(ehdr)) {
        return -1;
    }

    if (!elf_validate(&ehdr)) {
        return -1;
    }

    uint64_t *pml4 = vmm_create_address_space();
    if (pml4 == 0) {
        return -1;
    }

    task_t tracking_task;
    tracking_task.user_page_count = 0;
    uint64_t heap_start = 0;

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        size_t offset = ehdr.e_phoff + ((size_t)i * ehdr.e_phentsize);

        if (vfs_read(node, offset, sizeof(phdr), (uint8_t *)&phdr) !=
            sizeof(phdr)) {
            return -1;
        }

        if (phdr.p_type == PT_LOAD) {
            uint64_t segment_end = align_up(phdr.p_vaddr + phdr.p_memsz);
            if (segment_end > heap_start) {
                heap_start = segment_end;
            }

            if (!map_segment(node, pml4, &phdr, &tracking_task)) {
                return -1;
            }
        }
    }

    heap_start = align_up(heap_start);

    if (!map_user_stack(pml4, &tracking_task, has_arg ? arg_str : 0)) {
        return -1;
    }

    if (!map_signal_trampoline(pml4, &tracking_task)) {
        return -1;
    }

    if (!vmm_is_mapped(pml4, ehdr.e_entry) ||
        !vmm_is_mapped(pml4, ELF_USER_STACK_TOP - sizeof(uint64_t)) ||
        !vmm_is_mapped(pml4, SIGNAL_TRAMPOLINE_ADDR)) {
        return -1;
    }

    uint64_t arg_rdi = has_arg ? (ELF_USER_STACK_TOP - 256) : 0;
    int pid = scheduler_add_user_process(ehdr.e_entry, ELF_USER_STACK_TOP,
        pml4, out_task, arg_rdi, 0);
    if (pid < 0) {
        return -1;
    }

    task_t *task = scheduler_find_task((uint32_t)pid);
    if (task != 0) {
        copy_process_name(task, file_path);
        task->user_page_count = tracking_task.user_page_count;
        for (uint32_t i = 0; i < tracking_task.user_page_count; i++) {
            task->user_physical_pages[i] = tracking_task.user_physical_pages[i];
        }
        task->heap_start = heap_start;
        task->heap_end = heap_start;
    }

    return pid;
}
