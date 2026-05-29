#include "scheduler.h"

#include <stddef.h>

#include "memory.h"
#include "mutex.h"
#include "vfs.h"
#include "vmm.h"

#define MAX_TASKS 8
#define TASK_STACK_SIZE 4096
#define USER_STACK_SIZE 4096
#define KERNEL_CODE_SELECTOR 0x08ULL
#define KERNEL_DATA_SELECTOR 0x10ULL
#define USER_DATA_SELECTOR 0x23ULL
#define USER_CODE_SELECTOR 0x2BULL
#define INITIAL_RFLAGS 0x202ULL

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND 0x43
#define PIT_BASE_HZ 1193182U
#define SCHEDULER_HZ 100U

#define PIC1_COMMAND 0x20
#define PIC_EOI 0x20

static struct task_control_block tasks[MAX_TASKS];
static uint8_t task_stacks[MAX_TASKS][TASK_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t user_stacks[MAX_TASKS][USER_STACK_SIZE] __attribute__((aligned(4096)));
static uint32_t task_count;
static int current_task_index;
static struct task_control_block *current_task;
static mutex_t task_table_mutex;

struct interrupt_task_frame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t user_rsp;
    uint64_t ss;
};

extern void tss_set_rsp0(uint64_t rsp0);
extern vfs_node_t *kernel_stdin_node(void);
extern vfs_node_t *kernel_stdout_node(void);
extern vfs_node_t *kernel_stderr_node(void);

static void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void pit_init(void)
{
    uint16_t divisor = (uint16_t)(PIT_BASE_HZ / SCHEDULER_HZ);

    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFFU));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFFU));
}

static uint64_t *push_u64(uint64_t *stack, uint64_t value)
{
    stack--;
    *stack = value;
    return stack;
}

static uint64_t build_initial_stack(uint8_t *stack, uint64_t user_rsp,
    task_entry_t entry, uint64_t code_selector, uint64_t data_selector)
{
    uintptr_t top = ((uintptr_t)stack + TASK_STACK_SIZE) & ~0xFULL;
    uintptr_t entry_rsp = top - sizeof(uint64_t);
    uint64_t *sp = (uint64_t *)entry_rsp;

    *(uint64_t *)entry_rsp = 0;

    if (user_rsp == 0) {
        user_rsp = entry_rsp;
    }

    sp = push_u64(sp, data_selector);
    sp = push_u64(sp, user_rsp);
    sp = push_u64(sp, INITIAL_RFLAGS);
    sp = push_u64(sp, code_selector);
    sp = push_u64(sp, (uint64_t)entry);

    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);
    sp = push_u64(sp, 0);

    return (uint64_t)sp;
}

static uint64_t stack_top(uint8_t *stack)
{
    return ((uint64_t)stack + TASK_STACK_SIZE) & ~0xFULL;
}

static void set_task_name(task_t *task, const char *name)
{
    if (task == 0 || name == 0) {
        return;
    }

    uint32_t i = 0;
    while (name[i] != '\0' && i < TASK_NAME_MAX - 1U) {
        task->name[i] = name[i];
        i++;
    }
    task->name[i] = '\0';
}

static uint32_t signal_bit(int signum)
{
    return 1U << (uint32_t)signum;
}

static int is_supported_signal(int signum)
{
    return signum == SIGINT || signum == SIGKILL || signum == SIGTERM;
}

static void reset_signal_state(task_t *task)
{
    for (uint32_t i = 0; i < TASK_SIGNAL_COUNT; i++) {
        task->signal_handlers[i] = 0;
    }

    task->pending_signals = 0;
    task->active_signal = 0;
    task->signal_context.signum = 0;
}

static void map_user_page(uint64_t address)
{
    uint64_t page = address & ~(USER_STACK_SIZE - 1ULL);

    vmm_map(page, page, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
}

static void pic_send_eoi(void)
{
    outb(PIC1_COMMAND, PIC_EOI);
}

static void init_standard_fds(task_t *task)
{
    for (uint32_t i = 0; i < TASK_MAX_FDS; i++) {
        task->file_descriptors[i] = 0;
        task->fd_offsets[i] = 0;
    }

    task->file_descriptors[0] = kernel_stdin_node();
    task->file_descriptors[1] = kernel_stdout_node();
    task->file_descriptors[2] = kernel_stderr_node();
}

static void inherit_fds(task_t *task, task_t *parent)
{
    if (parent == 0) {
        init_standard_fds(task);
        return;
    }

    for (uint32_t i = 0; i < TASK_MAX_FDS; i++) {
        task->file_descriptors[i] = parent->file_descriptors[i];
        task->fd_offsets[i] = parent->fd_offsets[i];
    }
}

static void release_user_pages(task_t *task)
{
    for (uint32_t i = 0; i < task->user_page_count; i++) {
        pmm_free((void *)task->user_physical_pages[i]);
        task->user_physical_pages[i] = 0;
    }

    task->user_page_count = 0;
}

static int allocate_task_slot(void)
{
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_ZOMBIE) {
            release_user_pages(&tasks[i]);
            return (int)i;
        }
    }

    if (task_count >= MAX_TASKS) {
        return -1;
    }

    return (int)task_count++;
}

void scheduler_init(void)
{
    task_count = 0;
    current_task_index = -1;
    current_task = NULL;
    mutex_init(&task_table_mutex);
    pit_init();
}

static int scheduler_create_task_with_mode(task_entry_t entry, int user_mode)
{
    mutex_lock(&task_table_mutex);

    int slot = allocate_task_slot();
    if (slot < 0) {
        mutex_unlock(&task_table_mutex);
        return -1;
    }

    struct task_control_block *task = &tasks[slot];
    uint64_t user_rsp = 0;
    uint64_t code_selector = KERNEL_CODE_SELECTOR;
    uint64_t data_selector = KERNEL_DATA_SELECTOR;

    if (user_mode) {
        user_rsp = ((uint64_t)user_stacks[slot] + USER_STACK_SIZE) & ~0xFULL;
        code_selector = USER_CODE_SELECTOR;
        data_selector = USER_DATA_SELECTOR;
        map_user_page((uint64_t)entry);
        map_user_page((uint64_t)user_stacks[slot]);
        map_user_page(user_rsp - sizeof(uint64_t));
    }

    task->kernel_stack_top = stack_top(task_stacks[slot]);
    task->cr3 = (uint64_t)vmm_kernel_pml4();
    task->rsp = build_initial_stack(task_stacks[slot], user_rsp, entry,
        code_selector, data_selector);
    task->registers.rip = (uint64_t)entry;
    task->registers.rflags = INITIAL_RFLAGS;
    task->pid = (uint32_t)slot + 1;
    task->parent_pid = current_task != 0 ? current_task->pid : 0;
    set_task_name(task, user_mode ? "user-task" : "kernel-task");
    reset_signal_state(task);
    task->wait_target = 0;
    task->exit_status = 0;
    task->heap_start = 0;
    task->heap_end = 0;
    task->user_page_count = 0;
    task->state = TASK_READY;
    task->wait_reason = TASK_WAIT_NONE;
    task->mutex_wait_next = 0;
    inherit_fds(task, current_task);
    int pid = (int)task->pid;

    mutex_unlock(&task_table_mutex);
    return pid;
}

int scheduler_create_task(task_entry_t entry)
{
    return scheduler_create_task_with_mode(entry, 0);
}

int scheduler_create_user_task(task_entry_t entry)
{
    return scheduler_create_task_with_mode(entry, 1);
}

int scheduler_add_user_process(uint64_t entry, uint64_t user_rsp,
    uint64_t *pml4, task_t *out_task)
{
    if (pml4 == 0 || user_rsp == 0) {
        return -1;
    }

    mutex_lock(&task_table_mutex);

    int slot = allocate_task_slot();
    if (slot < 0) {
        mutex_unlock(&task_table_mutex);
        return -1;
    }

    task_t *task = &tasks[slot];
    task->kernel_stack_top = stack_top(task_stacks[slot]);
    task->cr3 = (uint64_t)pml4;
    task->rsp = build_initial_stack(task_stacks[slot], user_rsp,
        (task_entry_t)entry, USER_CODE_SELECTOR, USER_DATA_SELECTOR);
    task->registers.rip = entry;
    task->registers.rflags = INITIAL_RFLAGS;
    task->pid = (uint32_t)slot + 1;
    task->parent_pid = current_task != 0 ? current_task->pid : 0;
    set_task_name(task, "process");
    reset_signal_state(task);
    task->wait_target = 0;
    task->exit_status = 0;
    task->heap_start = 0;
    task->heap_end = 0;
    task->user_page_count = 0;
    task->state = TASK_READY;
    task->wait_reason = TASK_WAIT_NONE;
    task->mutex_wait_next = 0;
    inherit_fds(task, current_task);

    if (out_task != 0) {
        *out_task = *task;
    }

    int pid = (int)task->pid;

    mutex_unlock(&task_table_mutex);
    return pid;
}

task_t *scheduler_current_task(void)
{
    return current_task;
}

task_t *scheduler_find_task(uint32_t pid)
{
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].pid == pid && tasks[i].state != TASK_ZOMBIE) {
            return &tasks[i];
        }
    }

    return 0;
}

void scheduler_task_table_lock(void)
{
    mutex_lock(&task_table_mutex);
}

void scheduler_task_table_unlock(void)
{
    mutex_unlock(&task_table_mutex);
}

uint32_t scheduler_task_count(void)
{
    return task_count;
}

task_t *scheduler_task_at(uint32_t index)
{
    if (index >= task_count) {
        return 0;
    }

    return &tasks[index];
}

void scheduler_yield(void)
{
    __asm__ volatile ("pause");
}

int scheduler_send_signal(uint32_t pid, int signum)
{
    if (!is_supported_signal(signum)) {
        return -1;
    }

    mutex_lock(&task_table_mutex);
    task_t *task = scheduler_find_task(pid);
    if (task == 0) {
        mutex_unlock(&task_table_mutex);
        return -1;
    }

    task->pending_signals |= signal_bit(signum);
    if (task->state == TASK_SLEEPING || task->state == TASK_WAITING ||
        task->state == TASK_BLOCKED) {
        task->state = TASK_READY;
        task->wait_reason = TASK_WAIT_NONE;
        task->wait_target = 0;
    }

    mutex_unlock(&task_table_mutex);
    return 0;
}

void scheduler_sleep_current(enum task_wait_reason reason, uint64_t target)
{
    if (current_task == 0) {
        return;
    }

    current_task->state = reason == TASK_WAIT_CHILD ? TASK_WAITING :
        TASK_SLEEPING;
    current_task->wait_reason = reason;
    current_task->wait_target = target;
}

void scheduler_wake_stdin_readers(void)
{
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_SLEEPING &&
            tasks[i].wait_reason == TASK_WAIT_STDIN) {
            tasks[i].state = TASK_READY;
            tasks[i].wait_reason = TASK_WAIT_NONE;
            tasks[i].wait_target = 0;
        }
    }
}

void scheduler_wake_pipe_readers(void *pipe)
{
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_SLEEPING &&
            tasks[i].wait_reason == TASK_WAIT_PIPE_READ &&
            tasks[i].wait_target == (uint64_t)pipe) {
            tasks[i].state = TASK_READY;
            tasks[i].wait_reason = TASK_WAIT_NONE;
            tasks[i].wait_target = 0;
        }
    }
}

void scheduler_wake_pipe_writers(void *pipe)
{
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_SLEEPING &&
            tasks[i].wait_reason == TASK_WAIT_PIPE_WRITE &&
            tasks[i].wait_target == (uint64_t)pipe) {
            tasks[i].state = TASK_READY;
            tasks[i].wait_reason = TASK_WAIT_NONE;
            tasks[i].wait_target = 0;
        }
    }
}

int scheduler_wait_current(uint32_t pid)
{
    task_t *child = 0;

    if (current_task == 0) {
        return -1;
    }

    mutex_lock(&task_table_mutex);

    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].pid == pid) {
            child = &tasks[i];
            break;
        }
    }

    if (child == 0 || child->parent_pid != current_task->pid) {
        mutex_unlock(&task_table_mutex);
        return -1;
    }

    if (child->state == TASK_ZOMBIE) {
        mutex_unlock(&task_table_mutex);
        return 0;
    }

    scheduler_sleep_current(TASK_WAIT_CHILD, pid);
    mutex_unlock(&task_table_mutex);
    return 1;
}

static void wake_parent_waiters(uint32_t exited_pid)
{
    for (uint32_t i = 0; i < task_count; i++) {
        if ((tasks[i].state == TASK_SLEEPING ||
                tasks[i].state == TASK_WAITING) &&
            tasks[i].wait_reason == TASK_WAIT_CHILD &&
            tasks[i].wait_target == exited_pid) {
            tasks[i].state = TASK_READY;
            tasks[i].wait_reason = TASK_WAIT_NONE;
            tasks[i].wait_target = 0;
        }
    }
}

void scheduler_terminate_task(task_t *task, int status)
{
    if (task == 0 || task->state == TASK_ZOMBIE) {
        return;
    }

    mutex_lock(&task_table_mutex);

    if (task->state == TASK_ZOMBIE) {
        mutex_unlock(&task_table_mutex);
        return;
    }

    uint32_t exited_pid = task->pid;
    uint64_t exited_cr3 = task->cr3;
    uint64_t restore_cr3 = 0;

    if (task != current_task && current_task != 0 &&
        current_task->state != TASK_ZOMBIE) {
        restore_cr3 = current_task->cr3;
    }

    if (exited_cr3 != (uint64_t)vmm_kernel_pml4()) {
        vmm_switch_address_space(vmm_kernel_pml4());
    }

    task->exit_status = status;
    task->state = TASK_ZOMBIE;
    task->wait_reason = TASK_WAIT_NONE;
    task->wait_target = 0;
    task->pending_signals = 0;
    task->active_signal = 0;
    release_user_pages(task);
    vmm_destroy_address_space((uint64_t *)exited_cr3);
    task->cr3 = (uint64_t)vmm_kernel_pml4();
    wake_parent_waiters(exited_pid);

    if (restore_cr3 != 0) {
        vmm_switch_address_space((uint64_t *)restore_cr3);
    }

    mutex_unlock(&task_table_mutex);
}

void scheduler_exit_current(int status)
{
    if (current_task == 0) {
        return;
    }

    scheduler_terminate_task(current_task, status);
}

static int next_pending_signal(task_t *task)
{
    for (int signum = 1; signum < TASK_SIGNAL_COUNT; signum++) {
        if (task->pending_signals & signal_bit(signum)) {
            return signum;
        }
    }

    return 0;
}

static void save_signal_context(task_t *task,
    struct interrupt_task_frame *frame, int signum)
{
    task->signal_context.registers.r15 = frame->r15;
    task->signal_context.registers.r14 = frame->r14;
    task->signal_context.registers.r13 = frame->r13;
    task->signal_context.registers.r12 = frame->r12;
    task->signal_context.registers.r11 = frame->r11;
    task->signal_context.registers.r10 = frame->r10;
    task->signal_context.registers.r9 = frame->r9;
    task->signal_context.registers.r8 = frame->r8;
    task->signal_context.registers.rbp = frame->rbp;
    task->signal_context.registers.rdi = frame->rdi;
    task->signal_context.registers.rsi = frame->rsi;
    task->signal_context.registers.rdx = frame->rdx;
    task->signal_context.registers.rcx = frame->rcx;
    task->signal_context.registers.rbx = frame->rbx;
    task->signal_context.registers.rax = frame->rax;
    task->signal_context.registers.rip = frame->rip;
    task->signal_context.registers.rflags = frame->rflags;
    task->signal_context.user_rsp = frame->user_rsp;
    task->signal_context.cs = frame->cs;
    task->signal_context.ss = frame->ss;
    task->signal_context.signum = (uint32_t)signum;
}

static uint64_t signal_handler_stack(uint64_t user_rsp)
{
    uint64_t aligned_rsp = user_rsp & ~0xFULL;
    return aligned_rsp - sizeof(uint64_t);
}

static int deliver_pending_signal(task_t *task)
{
    if (task == 0 || task->active_signal != 0) {
        return 1;
    }

    int signum = next_pending_signal(task);
    if (signum == 0) {
        return 1;
    }

    uintptr_t handler = signum == SIGKILL ? 0 : task->signal_handlers[signum];
    if (handler == 0 || handler == (uintptr_t)-1) {
        // Forçar terminação padrão se o handler for inválido
        task->pending_signals &= ~signal_bit(signum);
        scheduler_terminate_task(task, -1);
        return 0;
    }

    struct interrupt_task_frame *frame =
        (struct interrupt_task_frame *)task->rsp;
    if ((frame->cs & 0x3ULL) != 0x3ULL) {
        return 1;
    }

    uint64_t user_rsp = signal_handler_stack(frame->user_rsp);

    // Garanta que o stack de Ring 3 esteja mapeado e acessível
    uint64_t stack_page = user_rsp & ~(4096ULL - 1ULL);
    if (!vmm_is_mapped((uint64_t *)task->cr3, stack_page)) {
        void *physical = pmm_alloc();
        if (physical == 0 || task->user_page_count >= TASK_MAX_USER_PAGES) {
            if (physical != 0) {
                pmm_free(physical);
            }
            scheduler_terminate_task(task, -1);
            return 0;
        }

        uint64_t *entries = (uint64_t *)physical;
        for (int i = 0; i < 512; i++) {
            entries[i] = 0;
        }

        task->user_physical_pages[task->user_page_count++] = (uint64_t)physical;
        vmm_map_in_space((uint64_t *)task->cr3, stack_page, (uintptr_t)physical,
            PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        vmm_flush(stack_page);
    }

    *(uint64_t *)user_rsp = SIGNAL_TRAMPOLINE_ADDR;

    save_signal_context(task, frame, signum);
    task->active_signal = (uint32_t)signum;
    task->pending_signals &= ~signal_bit(signum);

    frame->rip = handler;
    frame->user_rsp = user_rsp;
    frame->rdi = (uint64_t)signum;

    return 1;
}

uint64_t scheduler_tick(uint64_t current_rsp)
{
    if (task_count == 0) {
        pic_send_eoi();
        return current_rsp;
    }

    if (current_task != NULL) {
        current_task->rsp = current_rsp;
        if (current_task->state == TASK_RUNNING) {
            current_task->state = TASK_READY;
        }
    }

    uint32_t attempts = 0;
    uint32_t max_attempts = task_count * 2U;

    while (attempts < max_attempts) {
        current_task_index = (current_task_index + 1) % (int)task_count;
        attempts++;
        struct task_control_block *candidate = &tasks[current_task_index];
        if (candidate->state == TASK_READY || candidate->state == TASK_RUNNING) {
            current_task = candidate;
            current_task->state = TASK_RUNNING;
            tss_set_rsp0(current_task->kernel_stack_top);
            vmm_switch_address_space((uint64_t *)current_task->cr3);
            if (!deliver_pending_signal(current_task)) {
                attempts = 0;
                continue;
            }
            pic_send_eoi();
            return current_task->rsp;
        }
    }

    pic_send_eoi();
    return current_rsp;
}

struct syscall_frame {
    uint64_t r9;
    uint64_t r8;
    uint64_t r10;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r11; // rflags
    uint64_t rcx; // rip
    uint64_t user_rsp;
};

void scheduler_handle_syscall_signals(uint64_t syscall_frame_addr, uint64_t syscall_ret)
{
    task_t *task = current_task;
    if (task == 0 || task->active_signal != 0) {
        return;
    }

    int signum = next_pending_signal(task);
    if (signum == 0) {
        return;
    }

    uintptr_t handler = signum == SIGKILL ? 0 : task->signal_handlers[signum];
    if (handler == 0 || handler == (uintptr_t)-1) {
        task->pending_signals &= ~signal_bit(signum);
        scheduler_terminate_task(task, -1);
        for (;;) {
            __asm__ volatile ("sti; hlt");
        }
    }

    struct syscall_frame *frame = (struct syscall_frame *)syscall_frame_addr;
    uint64_t user_rsp = signal_handler_stack(frame->user_rsp);

    // Garanta que o stack de Ring 3 esteja mapeado e acessível
    uint64_t stack_page = user_rsp & ~(4096ULL - 1ULL);
    if (!vmm_is_mapped((uint64_t *)task->cr3, stack_page)) {
        void *physical = pmm_alloc();
        if (physical == 0 || task->user_page_count >= TASK_MAX_USER_PAGES) {
            if (physical != 0) {
                pmm_free(physical);
            }
            scheduler_terminate_task(task, -1);
            for (;;) {
                __asm__ volatile ("sti; hlt");
            }
        }

        uint64_t *entries = (uint64_t *)physical;
        for (int i = 0; i < 512; i++) {
            entries[i] = 0;
        }

        task->user_physical_pages[task->user_page_count++] = (uint64_t)physical;
        vmm_map_in_space((uint64_t *)task->cr3, stack_page, (uintptr_t)physical,
            PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        vmm_flush(stack_page);
    }

    *(uint64_t *)user_rsp = SIGNAL_TRAMPOLINE_ADDR;

    // Save context from the syscall frame.
    task->signal_context.registers.r15 = frame->r15;
    task->signal_context.registers.r14 = frame->r14;
    task->signal_context.registers.r13 = frame->r13;
    task->signal_context.registers.r12 = frame->r12;
    task->signal_context.registers.r11 = frame->r11;
    task->signal_context.registers.r10 = frame->r10;
    task->signal_context.registers.r9 = frame->r9;
    task->signal_context.registers.r8 = frame->r8;
    task->signal_context.registers.rbp = frame->rbp;
    task->signal_context.registers.rdi = frame->rdi;
    task->signal_context.registers.rsi = frame->rsi;
    task->signal_context.registers.rdx = frame->rdx;
    task->signal_context.registers.rcx = frame->rcx;
    task->signal_context.registers.rbx = frame->rbx;
    task->signal_context.registers.rax = syscall_ret;
    task->signal_context.registers.rip = frame->rcx;
    task->signal_context.registers.rflags = frame->r11;
    task->signal_context.user_rsp = frame->user_rsp;
    task->signal_context.cs = USER_CODE_SELECTOR;
    task->signal_context.ss = USER_DATA_SELECTOR;
    task->signal_context.signum = (uint32_t)signum;

    task->active_signal = (uint32_t)signum;
    task->pending_signals &= ~signal_bit(signum);

    // Redirect to signal handler
    frame->rcx = handler;
    frame->user_rsp = user_rsp;
    frame->rdi = (uint64_t)signum;
}
