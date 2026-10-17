#include "elf.h"

#include <stddef.h>

#include "heap.h"
#include "memory.h"
#include "scheduler.h"
#include "vfs.h"
#include "vmm.h"

#define ELF_USER_STACK_TOP 0x0000008000800000ULL
#define ELF_USER_STACK_PAGES 4ULL
#define PAGE_MASK (~(PMM_PAGE_SIZE - 1ULL))
#define EXEC_VECTOR_MAX 32U
#define EXEC_STRINGS_MAX 4096U
#define EXEC_STRING_MAX 256U
#define ELF_INITIAL_RFLAGS 0x202ULL

struct syscall_frame {
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r11, rcx, user_rsp;
};

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
        ehdr->e_ident[6] == 1 &&
        ehdr->e_type == ET_EXEC &&
        ehdr->e_version == 1 &&
        ehdr->e_ehsize == sizeof(Elf64_Ehdr) &&
        ehdr->e_machine == EM_X86_64 &&
        ehdr->e_phentsize == sizeof(Elf64_Phdr);
}

static uint64_t segment_page_flags(const Elf64_Phdr *phdr)
{
    uint64_t flags = PAGE_PRESENT | PAGE_USER;

    if (phdr->p_flags & PF_W) {
        flags |= PAGE_WRITABLE;
    }
    if ((phdr->p_flags & PF_X) == 0) flags |= PAGE_NX;

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

static int map_signal_trampoline(uint64_t *pml4, task_t *tracking_task);

static int map_segment(vfs_node_t *node, uint64_t *pml4, const Elf64_Phdr *phdr,
    task_t *tracking_task)
{
    if (phdr->p_filesz > phdr->p_memsz || phdr->p_offset > node->size ||
        phdr->p_filesz > node->size - phdr->p_offset ||
        phdr->p_vaddr > UINT64_MAX - phdr->p_memsz || phdr->p_memsz == 0 ||
        phdr->p_vaddr < VMM_USER_BASE ||
        phdr->p_vaddr + phdr->p_memsz > VMM_USER_LIMIT ||
        phdr->p_vaddr + phdr->p_memsz > UINT64_MAX - (PMM_PAGE_SIZE - 1ULL)) {
        return 0;
    }
    if (phdr->p_align > 1 &&
        ((phdr->p_align & (phdr->p_align - 1ULL)) != 0 ||
         phdr->p_vaddr % phdr->p_align != phdr->p_offset % phdr->p_align)) {
        return 0;
    }
    uint64_t segment_start = align_down(phdr->p_vaddr);
    uint64_t segment_end = align_up(phdr->p_vaddr + phdr->p_memsz);
    uint64_t flags = segment_page_flags(phdr);

    for (uint64_t page = segment_start; page < segment_end; page += PMM_PAGE_SIZE) {
        if (vmm_is_mapped(pml4, page)) return 0;
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

            uint64_t bytes = page_file_end - page_file_start;
            int read_bytes = vfs_read(node,
                phdr->p_offset + (page_file_start - phdr->p_vaddr), bytes,
                physical + (page_file_start - page));
            if (read_bytes != (int)bytes) {
                tracking_task->user_page_count--;
                pmm_free(physical);
                return 0;
            }
        }

        vmm_map_in_space(pml4, page, (uintptr_t)physical, flags);
        if (!vmm_is_mapped(pml4, page)) {
            tracking_task->user_page_count--;
            pmm_free(physical);
            return 0;
        }
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
        if (!vmm_is_mapped(pml4, page)) {
            tracking_task->user_page_count--;
            pmm_free(physical);
            return 0;
        }

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

static int copy_exec_vector(const char *const *user_vector, char *storage,
    size_t *storage_used, uint16_t *offsets, uint16_t *count, int environment)
{
    *count = 0;
    if (user_vector == 0) {
        return 1;
    }

    for (uint16_t i = 0; i <= EXEC_VECTOR_MAX; i++) {
        const char *user_string = 0;
        if (!vmm_validate_user_ptr(&user_vector[i], sizeof(user_string), 0)) {
            return 0;
        }
        user_string = user_vector[i];
        if (user_string == 0) {
            return 1;
        }
        if (i == EXEC_VECTOR_MAX || !vmm_validate_user_string(user_string,
                EXEC_STRING_MAX)) {
            return 0;
        }

        size_t length = 0;
        while (user_string[length] != '\0') {
            length++;
        }
        if (environment) {
            size_t name_length = 0;
            while (name_length < length && user_string[name_length] != '=')
                name_length++;
            if (name_length == 0 || name_length == length) return 0;
        }
        if (*storage_used + length + 1U > EXEC_STRINGS_MAX) {
            return 0;
        }
        offsets[i] = (uint16_t)*storage_used;
        for (size_t j = 0; j <= length; j++) {
            storage[*storage_used + j] = user_string[j];
        }
        *storage_used += length + 1U;
        *count = i + 1U;
    }
    return 0;
}

static int map_exec_stack(uint64_t *pml4, task_t *tracking_task,
    const char *storage, const uint16_t *arg_offsets, uint16_t argc,
    const uint16_t *env_offsets, uint16_t envc, uint64_t *initial_rsp,
    uint64_t *argv_addr, uint64_t *envp_addr)
{
    uint64_t bottom = ELF_USER_STACK_TOP - ELF_USER_STACK_PAGES * PMM_PAGE_SIZE;
    for (uint64_t page = bottom; page < ELF_USER_STACK_TOP; page += PMM_PAGE_SIZE) {
        uint8_t *physical = pmm_alloc();
        if (physical == 0 || !track_user_page(tracking_task, physical)) {
            if (physical != 0) pmm_free(physical);
            return 0;
        }
        memory_zero(physical, PMM_PAGE_SIZE);
        vmm_map_in_space(pml4, page, (uintptr_t)physical,
            PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        if (!vmm_is_mapped(pml4, page)) {
            tracking_task->user_page_count--;
            pmm_free(physical);
            return 0;
        }
    }
    if (!map_signal_trampoline(pml4, tracking_task)) {
        return 0;
    }

    uint64_t cursor = ELF_USER_STACK_TOP;
    uint64_t arg_ptrs[EXEC_VECTOR_MAX];
    uint64_t env_ptrs[EXEC_VECTOR_MAX];
    for (uint16_t i = 0; i < envc; i++) {
        size_t len = 0;
        while (storage[env_offsets[i] + len] != '\0') len++;
        cursor -= len + 1U;
        env_ptrs[i] = cursor;
    }
    for (uint16_t i = 0; i < argc; i++) {
        size_t len = 0;
        while (storage[arg_offsets[i] + len] != '\0') len++;
        cursor -= len + 1U;
        arg_ptrs[i] = cursor;
    }
    cursor &= ~0xFULL;
    if (((argc + envc) & 1U) != 0) cursor -= sizeof(uint64_t);
    uint64_t env_array = cursor - (uint64_t)(envc + 1U) * sizeof(uint64_t);
    uint64_t arg_array = env_array - (uint64_t)(argc + 1U) * sizeof(uint64_t);
    uint64_t rsp = arg_array - sizeof(uint64_t);
    if (rsp < bottom + 640U) {
        return 0;
    }

    vmm_switch_address_space(pml4);
    for (uint16_t i = 0; i < envc; i++) {
        size_t len = 0;
        while (storage[env_offsets[i] + len] != '\0') len++;
        for (size_t j = 0; j <= len; j++) {
            ((char *)env_ptrs[i])[j] = storage[env_offsets[i] + j];
        }
        ((uint64_t *)env_array)[i] = env_ptrs[i];
    }
    ((uint64_t *)env_array)[envc] = 0;
    for (uint16_t i = 0; i < argc; i++) {
        size_t len = 0;
        while (storage[arg_offsets[i] + len] != '\0') len++;
        for (size_t j = 0; j <= len; j++) {
            ((char *)arg_ptrs[i])[j] = storage[arg_offsets[i] + j];
        }
        ((uint64_t *)arg_array)[i] = arg_ptrs[i];
    }
    ((uint64_t *)arg_array)[argc] = 0;
    ((uint64_t *)rsp)[0] = argc;
    vmm_switch_address_space(vmm_kernel_pml4());

    *initial_rsp = rsp;
    *argv_addr = arg_array;
    *envp_addr = env_array;
    return 1;
}

int elf_exec_current(const char *path, const char *const *argv,
    const char *const *envp, uint64_t syscall_frame_addr)
{
    task_t *task = scheduler_current_task();
    char file_path[256];
    char *storage = 0;
    uint16_t arg_offsets[EXEC_VECTOR_MAX] = {0};
    uint16_t env_offsets[EXEC_VECTOR_MAX] = {0};
    size_t storage_used = 0;
    uint16_t argc = 0, envc = 0;
    if (task == 0 || syscall_frame_addr == 0 ||
        !vmm_validate_user_string(path, sizeof(file_path)) ||
        (storage = kmalloc(EXEC_STRINGS_MAX)) == 0 ||
        !copy_exec_vector(argv, storage, &storage_used, arg_offsets, &argc, 0) ||
        !copy_exec_vector(envp, storage, &storage_used, env_offsets, &envc, 1)) {
        if (storage != 0) kfree(storage);
        return -1;
    }
    size_t path_len = 0;
    while (path[path_len] != '\0') path_len++;
    if (path_len == 0 || path_len >= sizeof(file_path)) {
        kfree(storage);
        return -1;
    }
    for (size_t i = 0; i <= path_len; i++) file_path[i] = path[i];

    uint64_t *old_pml4 = (uint64_t *)task->cr3;

    vmm_switch_address_space(vmm_kernel_pml4());
    vfs_node_t *node = vfs_find(file_path);
    if (node == 0 || node->type != VFS_NODE_FILE) goto fail_no_space;
    Elf64_Ehdr ehdr;
    if (node->size < sizeof(ehdr) ||
        vfs_read(node, 0, sizeof(ehdr), (uint8_t *)&ehdr) != sizeof(ehdr) ||
        !elf_validate(&ehdr) || ehdr.e_ehsize != sizeof(ehdr) ||
        ehdr.e_phnum == 0 || ehdr.e_phnum > 64 ||
        ehdr.e_phoff > node->size ||
        (uint64_t)ehdr.e_phnum * ehdr.e_phentsize > node->size - ehdr.e_phoff) {
        goto fail_no_space;
    }

    uint64_t *pml4 = vmm_create_address_space();
    if (pml4 == 0) goto fail_no_space;
    task_t image;
    image.user_page_count = 0;
    uint64_t heap_start = 0;
    int entry_executable = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        uint64_t offset = ehdr.e_phoff + (uint64_t)i * ehdr.e_phentsize;
        if (vfs_read(node, offset, sizeof(phdr), (uint8_t *)&phdr) != sizeof(phdr))
            goto fail_image;
        if (phdr.p_type != PT_LOAD) continue;
        if (phdr.p_filesz > phdr.p_memsz || phdr.p_offset > node->size ||
            phdr.p_filesz > node->size - phdr.p_offset ||
            phdr.p_vaddr < VMM_USER_BASE || phdr.p_memsz == 0 ||
            phdr.p_vaddr > VMM_USER_LIMIT || phdr.p_memsz > VMM_USER_LIMIT - phdr.p_vaddr ||
            phdr.p_vaddr + phdr.p_memsz > ELF_USER_STACK_TOP - ELF_USER_STACK_PAGES * PMM_PAGE_SIZE ||
            phdr.p_vaddr + phdr.p_memsz > SIGNAL_TRAMPOLINE_ADDR) goto fail_image;
        if ((phdr.p_flags & PF_X) && ehdr.e_entry >= phdr.p_vaddr &&
            ehdr.e_entry < phdr.p_vaddr + phdr.p_memsz) entry_executable = 1;
        uint64_t end = align_up(phdr.p_vaddr + phdr.p_memsz);
        if (end > heap_start) heap_start = end;
        if (!map_segment(node, pml4, &phdr, &image)) goto fail_image;
    }
    if (!entry_executable || image.user_page_count > TASK_MAX_USER_PAGES - ELF_USER_STACK_PAGES - 1)
        goto fail_image;
    uint64_t user_rsp, argv_addr, envp_addr;
    if (!map_exec_stack(pml4, &image, storage, arg_offsets, argc,
            env_offsets, envc, &user_rsp, &argv_addr, &envp_addr)) goto fail_image;
    kfree(storage);
    storage = 0;

    struct syscall_frame *frame = (struct syscall_frame *)syscall_frame_addr;
    task->cr3 = (uint64_t)pml4;
    task->user_page_count = image.user_page_count;
    for (uint32_t i = 0; i < image.user_page_count; i++)
        task->user_physical_pages[i] = image.user_physical_pages[i];
    task->heap_start = heap_start;
    task->heap_end = heap_start;
    copy_process_name(task, file_path);
    for (size_t i = 0; i < TASK_SIGNAL_COUNT; i++) {
        if (task->signal_handlers[i] != SIG_IGN)
            task->signal_handlers[i] = SIG_DFL;
    }
    task->active_signal = 0;
    memory_zero(&task->signal_context, sizeof(task->signal_context));
    memory_zero(&task->sigreturn_frame, sizeof(task->sigreturn_frame));
    frame->r9 = frame->r8 = frame->r10 = frame->rdx = 0;
    frame->rsi = argv_addr;
    frame->rdi = argc;
    frame->rdx = envp_addr;
    frame->r11 = ELF_INITIAL_RFLAGS;
    frame->r15 = frame->r14 = frame->r13 = frame->r12 = frame->rbp = frame->rbx = 0;
    frame->rcx = ehdr.e_entry;
    frame->user_rsp = user_rsp;
    memory_zero(&task->registers, sizeof(task->registers));
    task->registers.rip = ehdr.e_entry;
    task->registers.rflags = ELF_INITIAL_RFLAGS;
    task->registers.rdi = argc;
    task->registers.rsi = argv_addr;
    task->registers.rdx = envp_addr;
    vmm_switch_address_space(pml4);
    vmm_destroy_address_space(old_pml4);
    return 0;

fail_image:
    vmm_destroy_address_space(pml4);
fail_no_space:
    if (storage != 0) kfree(storage);
    vmm_switch_address_space(old_pml4);
    return -1;
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
    if (!vmm_is_mapped(pml4, SIGNAL_TRAMPOLINE_ADDR)) {
        tracking_task->user_page_count--;
        pmm_free(physical);
        return 0;
    }
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
    if (node->size < sizeof(ehdr) ||
        vfs_read(node, 0, sizeof(ehdr), (uint8_t *)&ehdr) != sizeof(ehdr)) {
        return -1;
    }

    if (!elf_validate(&ehdr) || ehdr.e_phnum == 0 || ehdr.e_phnum > 64 ||
        ehdr.e_phoff > node->size ||
        (uint64_t)ehdr.e_phnum * ehdr.e_phentsize > node->size - ehdr.e_phoff) {
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
            goto fail_image;
        }

        if (phdr.p_type == PT_LOAD) {
            if (!map_segment(node, pml4, &phdr, &tracking_task)) {
                goto fail_image;
            }
            uint64_t segment_end = align_up(phdr.p_vaddr + phdr.p_memsz);
            if (segment_end > heap_start) {
                heap_start = segment_end;
            }
        }
    }

    heap_start = align_up(heap_start);

    if (!map_user_stack(pml4, &tracking_task, has_arg ? arg_str : 0)) {
        goto fail_image;
    }

    if (!map_signal_trampoline(pml4, &tracking_task)) {
        goto fail_image;
    }

    if (!vmm_is_mapped(pml4, ehdr.e_entry) ||
        !vmm_is_mapped(pml4, ELF_USER_STACK_TOP - sizeof(uint64_t)) ||
        !vmm_is_mapped(pml4, SIGNAL_TRAMPOLINE_ADDR)) {
        goto fail_image;
    }

    uint64_t user_stack_top = has_arg ? (ELF_USER_STACK_TOP - 256) : ELF_USER_STACK_TOP;
    uint64_t arg_rdi = has_arg ? (ELF_USER_STACK_TOP - 256) : 0;
    int pid = scheduler_add_user_process(ehdr.e_entry, user_stack_top,
        pml4, out_task, arg_rdi, 0);
    if (pid < 0) {
        goto fail_image;
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
        if (out_task != 0) *out_task = *task;
    }

    return pid;

fail_image:
    vmm_destroy_address_space(pml4);
    return -1;
}
