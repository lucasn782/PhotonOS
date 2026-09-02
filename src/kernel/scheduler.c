#include "scheduler.h"

#include <stddef.h>

#include "memory.h"
#include "mutex.h"
#include "serial.h"
#include "vfs.h"
#include "vmm.h"
#include "heap.h"
#include "apic.h"
#include "smp.h"

extern volatile uint64_t kernel_ticks;

#define MAX_TASKS 16
#define TASK_STACK_SIZE 8192
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

static struct task_control_block *tasks;
static uint8_t (*task_stacks)[TASK_STACK_SIZE];
static uint8_t (*user_stacks)[USER_STACK_SIZE];
static uint8_t *user_stacks_raw;
static uint32_t task_count;
static int current_task_index;
static int idle_task_index;
static struct task_control_block *current_task;
static struct task_control_block kernel_task;
static spinlock_t task_table_lock;
static volatile uint64_t task_table_flags[256];

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

static int scheduler_create_task_with_mode(task_entry_t entry, int user_mode);

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
    task_entry_t entry, uint64_t code_selector, uint64_t data_selector,
    uint64_t arg_rdi, uint64_t arg_rsi)
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

    sp = push_u64(sp, 0); // rax
    sp = push_u64(sp, 0); // rbx
    sp = push_u64(sp, 0); // rcx
    sp = push_u64(sp, 0); // rdx
    sp = push_u64(sp, arg_rsi); // rsi
    sp = push_u64(sp, arg_rdi); // rdi
    sp = push_u64(sp, 0); // rbp
    sp = push_u64(sp, 0); // r8
    sp = push_u64(sp, 0); // r9
    sp = push_u64(sp, 0); // r10
    sp = push_u64(sp, 0); // r11
    sp = push_u64(sp, 0); // r12
    sp = push_u64(sp, 0); // r13
    sp = push_u64(sp, 0); // r14
    sp = push_u64(sp, 0); // r15

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

static uint32_t next_pid = 1;

static uint32_t signal_bit(int signum)
{
    return 1U << (uint32_t)signum;
}

static int is_supported_signal(int signum)
{
    return signum > 0 && signum < TASK_SIGNAL_COUNT;
}

static void reset_signal_state(task_t *task)
{
    for (uint32_t i = 0; i < TASK_SIGNAL_COUNT; i++) {
        task->signal_handlers[i] = 0;
    }

    task->pending_signals = 0;
    task->blocked_signals = 0;
    task->active_signal = 0;
    task->signal_context.signum = 0;
}

static void map_user_page(uint64_t address)
{
    uint64_t page = address & ~(USER_STACK_SIZE - 1ULL);
    uint64_t phys = vmm_virt_to_phys(page);
    if (phys == 0) {
        phys = page;
    }

    vmm_map(page, phys, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
}

static void pic_send_eoi(void)
{
    outb(PIC1_COMMAND, PIC_EOI);
    if (apic_is_enabled()) {
        apic_eoi();
    }
}

static void scheduler_idle_thread(void)
{
    for (;;) {
        __asm__ volatile ("sti" ::: "memory");
        __asm__ volatile ("hlt" ::: "memory");
    }
}

static void init_standard_fds(task_t *task)
{
    for (uint32_t i = 0; i < TASK_MAX_FDS; i++) {
        task->file_descriptors[i] = 0;
    }

    task->file_descriptors[0] = file_description_alloc(kernel_stdin_node(), 0);
    task->file_descriptors[1] = file_description_alloc(kernel_stdout_node(), 0);
    task->file_descriptors[2] = file_description_alloc(kernel_stderr_node(), 0);
}

static void inherit_fds(task_t *task, task_t *parent)
{
    if (parent == 0) {
        init_standard_fds(task);
        return;
    }

    for (uint32_t i = 0; i < TASK_MAX_FDS; i++) {
        task->file_descriptors[i] = parent->file_descriptors[i];
        if (task->file_descriptors[i] != 0) {
            task->file_descriptors[i]->ref_count++;
        }
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
        if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_DEAD) {
            release_user_pages(&tasks[i]);
            return (int)i;
        }
    }

    if (task_count >= MAX_TASKS) {
        return -1;
    }

    return (int)task_count++;
}

static uint32_t generate_pid(void)
{
    uint32_t pid = next_pid;
    for (;;) {
        if (pid == 0) pid = 1;
        int in_use = 0;
        for (uint32_t i = 0; i < task_count; i++) {
            if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_DEAD && tasks[i].pid == pid) {
                in_use = 1;
                break;
            }
        }
        if (!in_use) {
            next_pid = pid + 1;
            return pid;
        }
        pid++;
    }
}

void scheduler_init(void)
{
    tasks = kmalloc(MAX_TASKS * sizeof(struct task_control_block));
    task_stacks = kmalloc((MAX_TASKS + 1) * TASK_STACK_SIZE);
    task_stacks = (uint8_t (*)[TASK_STACK_SIZE])(((uintptr_t)task_stacks + TASK_STACK_SIZE - 1) & ~(TASK_STACK_SIZE - 1));
    user_stacks_raw = kmalloc((MAX_TASKS + 1) * USER_STACK_SIZE);
    user_stacks = (uint8_t (*)[USER_STACK_SIZE])(((uintptr_t)user_stacks_raw + USER_STACK_SIZE - 1) & ~(USER_STACK_SIZE - 1));

    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_UNUSED;
        tasks[i].pid = 0;
    }

    task_count = 0;
    current_task_index = -1;
    idle_task_index = -1;
    current_task = NULL;
    spin_init(&task_table_lock);
    pit_init();

    int idle_pid = scheduler_create_task(scheduler_idle_thread);
    if (idle_pid >= 0) {
        idle_task_index = idle_pid - 1;
        set_task_name(&tasks[idle_task_index], "idle");
    }
}

static int scheduler_create_task_with_mode(task_entry_t entry, int user_mode)
{
    uint64_t flags = spin_lock_irqsave(&task_table_lock);

    int slot = allocate_task_slot();
    if (slot < 0) {
        spin_unlock_irqrestore(&task_table_lock, flags);
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

    task->guard_page = 0;
    task->kernel_stack_top = stack_top(task_stacks[slot]);
    task->cr3 = (uint64_t)vmm_kernel_pml4();
    task->rsp = build_initial_stack(task_stacks[slot], user_rsp, entry,
        code_selector, data_selector, 0, 0);
    task->registers.rip = (uint64_t)entry;
    task->registers.rflags = INITIAL_RFLAGS;
    task->pid = generate_pid();
    task->parent_pid = current_task != 0 ? current_task->pid : 0;
    task->uid = current_task != 0 ? current_task->uid : 0;
    task->gid = current_task != 0 ? current_task->gid : 0;
    task->umask = current_task != 0 ? current_task->umask : 0022;
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
    task->cwd[0] = '/';
    task->cwd[1] = '\0';
    inherit_fds(task, current_task);
    int pid = (int)task->pid;

    spin_unlock_irqrestore(&task_table_lock, flags);
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
    uint64_t *pml4, task_t *out_task, uint64_t arg_rdi, uint64_t arg_rsi)
{
    if (pml4 == 0 || user_rsp == 0) {
        return -1;
    }

    uint64_t flags = spin_lock_irqsave(&task_table_lock);

    int slot = allocate_task_slot();
    if (slot < 0) {
        spin_unlock_irqrestore(&task_table_lock, flags);
        return -1;
    }

    task_t *task = &tasks[slot];
    task->guard_page = 0;
    task->kernel_stack_top = stack_top(task_stacks[slot]);
    task->cr3 = (uint64_t)pml4;
    task->rsp = build_initial_stack(task_stacks[slot], user_rsp,
        (task_entry_t)entry, USER_CODE_SELECTOR, USER_DATA_SELECTOR, arg_rdi, arg_rsi);
    task->registers.rip = entry;
    task->registers.rflags = INITIAL_RFLAGS;
    task->pid = generate_pid();
    task->parent_pid = current_task != 0 ? current_task->pid : 0;
    task->uid = current_task != 0 ? current_task->uid : 0;
    task->gid = current_task != 0 ? current_task->gid : 0;
    task->umask = current_task != 0 ? current_task->umask : 0022;
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
    if (current_task != 0 && current_task->cwd[0] != '\0') {
        size_t ci = 0;
        while (current_task->cwd[ci] != '\0' && ci < sizeof(task->cwd) - 1) {
            task->cwd[ci] = current_task->cwd[ci];
            ci++;
        }
        task->cwd[ci] = '\0';
    } else {
        task->cwd[0] = '/';
        task->cwd[1] = '\0';
    }
    inherit_fds(task, current_task);

    if (out_task != 0) {
        *out_task = *task;
    }

    int pid = (int)task->pid;

    spin_unlock_irqrestore(&task_table_lock, flags);
    return pid;
}

task_t *scheduler_current_task(void)
{
    if (current_task != 0) {
        return current_task;
    }
    return &kernel_task;
}

task_t *scheduler_find_task(uint32_t pid)
{
    uint64_t flags = spin_lock_irqsave(&task_table_lock);
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_DEAD && tasks[i].pid == pid) {
            spin_unlock_irqrestore(&task_table_lock, flags);
            return &tasks[i];
        }
    }
    spin_unlock_irqrestore(&task_table_lock, flags);
    return 0;
}

void scheduler_task_table_lock(void)
{
    uint32_t cpu_id = apic_read(0x20) >> 24;
    task_table_flags[cpu_id] = spin_lock_irqsave(&task_table_lock);
}

void scheduler_task_table_unlock(void)
{
    uint32_t cpu_id = apic_read(0x20) >> 24;
    spin_unlock_irqrestore(&task_table_lock, task_table_flags[cpu_id]);
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
    __asm__ volatile ("int $0x20" ::: "memory");
}

static void scheduler_terminate_task_unlocked(task_t *task, int status);

int scheduler_send_signal(uint32_t pid, int signum)
{
    if (!is_supported_signal(signum)) {
        return -1;
    }

    uint64_t flags = spin_lock_irqsave(&task_table_lock);
    task_t *task = 0;
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_DEAD && tasks[i].pid == pid) {
            task = &tasks[i];
            break;
        }
    }
    if (task == 0 || task->state == TASK_ZOMBIE || task->state == TASK_DEAD) {
        spin_unlock_irqrestore(&task_table_lock, flags);
        return -1;
    }

    if (signum == SIGKILL) {
        scheduler_terminate_task_unlocked(task, 128 + SIGKILL);
        spin_unlock_irqrestore(&task_table_lock, flags);
        return 0;
    }

    if (signum == SIGSTOP) {
        task->pending_signals &= ~signal_bit(SIGCONT);
        task->state = TASK_STOPPED;
        task->wait_reason = TASK_WAIT_NONE;
        task->wait_target = 0;
        uint32_t parent = task->parent_pid;
        spin_unlock_irqrestore(&task_table_lock, flags);
        if (parent != 0) {
            scheduler_send_signal(parent, SIGCHLD);
        }
        return 0;
    }

    if (signum == SIGCONT) {
        task->pending_signals &= ~signal_bit(SIGSTOP);
        if (task->state == TASK_STOPPED) {
            task->state = TASK_READY;
            task->wait_reason = TASK_WAIT_NONE;
            task->wait_target = 0;
        }
        if (task->signal_handlers[SIGCONT] == (uintptr_t)0 || task->signal_handlers[SIGCONT] == (uintptr_t)1) {
            spin_unlock_irqrestore(&task_table_lock, flags);
            return 0;
        }
    }

    if (signum == SIGCHLD) {
        if (task->signal_handlers[SIGCHLD] == (uintptr_t)0 || task->signal_handlers[SIGCHLD] == (uintptr_t)1) {
            spin_unlock_irqrestore(&task_table_lock, flags);
            return 0;
        }
    }

    task->pending_signals |= signal_bit(signum);
    if ((task->state == TASK_SLEEPING || task->state == TASK_WAITING ||
        task->state == TASK_BLOCKED) && !(task->blocked_signals & signal_bit(signum))) {
        task->state = TASK_READY;
        task->wait_reason = TASK_WAIT_NONE;
        task->wait_target = 0;
    }

    spin_unlock_irqrestore(&task_table_lock, flags);
    return 0;
}

int scheduler_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    if (signum <= 0 || signum >= TASK_SIGNAL_COUNT || signum == SIGKILL || signum == SIGSTOP) {
        return -1;
    }

    task_t *task = scheduler_current_task();
    if (task == 0) return -1;

    uint64_t flags = spin_lock_irqsave(&task_table_lock);

    if (oldact != 0) {
        oldact->sa_handler = (sighandler_t)task->signal_handlers[signum];
        oldact->sa_mask = task->blocked_signals;
        oldact->sa_flags = 0;
    }

    if (act != 0) {
        task->signal_handlers[signum] = (uintptr_t)act->sa_handler;
    }

    spin_unlock_irqrestore(&task_table_lock, flags);
    return 0;
}

int scheduler_sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    task_t *task = scheduler_current_task();
    if (task == 0) return -1;

    uint64_t flags = spin_lock_irqsave(&task_table_lock);

    if (oldset != 0) {
        *oldset = task->blocked_signals;
    }

    if (set != 0) {
        sigset_t new_set = *set;
        new_set &= ~signal_bit(SIGKILL);
        new_set &= ~signal_bit(SIGSTOP);

        if (how == SIG_BLOCK) {
            task->blocked_signals |= new_set;
        } else if (how == SIG_UNBLOCK) {
            task->blocked_signals &= ~new_set;
        } else if (how == SIG_SETMASK) {
            task->blocked_signals = new_set;
        } else {
            spin_unlock_irqrestore(&task_table_lock, flags);
            return -1;
        }
    }

    spin_unlock_irqrestore(&task_table_lock, flags);
    return 0;
}

void scheduler_sleep_current(enum task_wait_reason reason, uint64_t target)
{
    if (current_task == 0) {
        return;
    }

    if (reason == TASK_WAIT_CHILD) {
        current_task->state = TASK_WAITING;
    } else if (reason == TASK_WAIT_SOCKET_RECV) {
        current_task->state = TASK_BLOCKED;
    } else {
        current_task->state = TASK_SLEEPING;
    }
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

void scheduler_wake_socket_receivers(uint8_t protocol)
{
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_BLOCKED &&
            tasks[i].wait_reason == TASK_WAIT_SOCKET_RECV &&
            tasks[i].wait_target == (uint64_t)protocol) {
            tasks[i].state = TASK_READY;
            tasks[i].wait_reason = TASK_WAIT_NONE;
            tasks[i].wait_target = 0;
        }
    }
}

void scheduler_wake_socket(void *sock)
{
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state == TASK_BLOCKED &&
            (tasks[i].wait_reason == TASK_WAIT_SOCKET_RECV ||
             tasks[i].wait_reason == TASK_WAIT_NETWORK) &&
            tasks[i].wait_target == (uint64_t)sock) {
            tasks[i].state = TASK_READY;
            tasks[i].wait_reason = TASK_WAIT_NONE;
            tasks[i].wait_target = 0;
        }
    }
}

int scheduler_waitpid(int32_t pid, int *status, int options)
{
    if (current_task == 0) {
        return -1;
    }

    uint64_t flags = spin_lock_irqsave(&task_table_lock);

    for (;;) {
        task_t *child = 0;
        int has_matching_children = 0;

        for (uint32_t i = 0; i < task_count; i++) {
            if (tasks[i].state == TASK_UNUSED || tasks[i].state == TASK_DEAD) {
                continue;
            }
            if (tasks[i].parent_pid != current_task->pid) {
                continue;
            }
            if (pid > 0 && tasks[i].pid != (uint32_t)pid) {
                continue;
            }

            has_matching_children = 1;

            if (tasks[i].state == TASK_ZOMBIE) {
                child = &tasks[i];
                break;
            }
        }

        if (child != 0) {
            uint32_t child_pid = child->pid;
            int exit_code = child->exit_status;
            child->state = TASK_DEAD;
            child->pid = 0;
            child->parent_pid = 0;
            spin_unlock_irqrestore(&task_table_lock, flags);

            if (status != 0) {
                *status = exit_code;
            }
            return (int)child_pid;
        }

        if (!has_matching_children) {
            spin_unlock_irqrestore(&task_table_lock, flags);
            return -1;
        }

        if (options & WNOHANG) {
            spin_unlock_irqrestore(&task_table_lock, flags);
            return 0;
        }

        current_task->state = TASK_WAITING;
        current_task->wait_reason = TASK_WAIT_CHILD;
        current_task->wait_target = pid > 0 ? (uint64_t)pid : 0;
        spin_unlock_irqrestore(&task_table_lock, flags);
        scheduler_yield();
        flags = spin_lock_irqsave(&task_table_lock);
    }
}

int scheduler_wait_current(uint32_t pid)
{
    int status = 0;
    return scheduler_waitpid((int32_t)pid, &status, 0);
}

static void wake_parent_waiters(uint32_t exited_pid)
{
    for (uint32_t i = 0; i < task_count; i++) {
        if ((tasks[i].state == TASK_SLEEPING ||
                tasks[i].state == TASK_WAITING) &&
            tasks[i].wait_reason == TASK_WAIT_CHILD &&
            (tasks[i].wait_target == exited_pid || tasks[i].wait_target == 0 || tasks[i].wait_target == (uint64_t)-1)) {
            tasks[i].state = TASK_READY;
            tasks[i].wait_reason = TASK_WAIT_NONE;
            tasks[i].wait_target = 0;
        }
    }
}

static void scheduler_terminate_task_unlocked(task_t *task, int status)
{
    if (task == 0 || task->state == TASK_ZOMBIE || task->state == TASK_DEAD || task->state == TASK_UNUSED) {
        return;
    }

    uint32_t exited_pid = task->pid;
    uint32_t parent_pid = task->parent_pid;
    uint64_t exited_cr3 = task->cr3;
    uint64_t restore_cr3 = 0;

    if (task != current_task && current_task != 0 &&
        current_task->state != TASK_ZOMBIE && current_task->state != TASK_DEAD && current_task->state != TASK_UNUSED) {
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
    task_close_all_fds(task);
    release_user_pages(task);
    vmm_destroy_address_space((uint64_t *)exited_cr3);
    task->cr3 = (uint64_t)vmm_kernel_pml4();

    /* Reparent children to PID 1 (shell / init) */
    for (uint32_t i = 0; i < task_count; i++) {
        if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_DEAD && tasks[i].parent_pid == exited_pid) {
            tasks[i].parent_pid = 1;
        }
    }

    wake_parent_waiters(exited_pid);

    if (restore_cr3 != 0) {
        vmm_switch_address_space((uint64_t *)restore_cr3);
    }

    /* Send SIGCHLD to parent */
    if (parent_pid != 0) {
        for (uint32_t i = 0; i < task_count; i++) {
            if (tasks[i].state != TASK_UNUSED && tasks[i].state != TASK_DEAD && tasks[i].pid == parent_pid) {
                if (tasks[i].signal_handlers[SIGCHLD] != (uintptr_t)0 && tasks[i].signal_handlers[SIGCHLD] != (uintptr_t)1) {
                    tasks[i].pending_signals |= signal_bit(SIGCHLD);
                }
                break;
            }
        }
    }
}

void scheduler_terminate_task(task_t *task, int status)
{
    if (task == 0 || task->state == TASK_ZOMBIE || task->state == TASK_DEAD || task->state == TASK_UNUSED) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&task_table_lock);

    if (task->state == TASK_ZOMBIE || task->state == TASK_DEAD || task->state == TASK_UNUSED) {
        spin_unlock_irqrestore(&task_table_lock, flags);
        return;
    }

    scheduler_terminate_task_unlocked(task, status);

    spin_unlock_irqrestore(&task_table_lock, flags);
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
    uint32_t deliverable = task->pending_signals & ~task->blocked_signals;
    for (int signum = 1; signum < TASK_SIGNAL_COUNT; signum++) {
        if (deliverable & signal_bit(signum)) {
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

static int deliver_pending_signal_unlocked(task_t *task)
{
    if (task == 0 || task->active_signal != 0) {
        return 1;
    }

    int signum = next_pending_signal(task);
    if (signum == 0) {
        return 1;
    }

    if (signum == SIGCHLD || signum == SIGCONT) {
        uintptr_t h = task->signal_handlers[signum];
        if (h == (uintptr_t)0 || h == (uintptr_t)1) {
            task->pending_signals &= ~signal_bit(signum);
            return 1;
        }
    }

    if (signum == SIGPIPE || signum == SIGINT || signum == SIGTERM) {
        uintptr_t h = task->signal_handlers[signum];
        if (h == (uintptr_t)1) {
            task->pending_signals &= ~signal_bit(signum);
            return 1;
        }
    }

    uintptr_t handler = signum == SIGKILL ? 0 : task->signal_handlers[signum];
    if (handler == 0 || handler == (uintptr_t)-1) {
        task->pending_signals &= ~signal_bit(signum);
        scheduler_terminate_task_unlocked(task, 128 + signum);
        return 0;
    }

    struct interrupt_task_frame *frame =
        (struct interrupt_task_frame *)task->rsp;
    if ((frame->cs & 0x3ULL) != 0x3ULL) {
        return 1;
    }

    uint64_t user_rsp = signal_handler_stack(frame->user_rsp);
    if (user_rsp >= 0x0000800000000000ULL || user_rsp < 0x1000) {
        task->pending_signals &= ~signal_bit(signum);
        scheduler_terminate_task_unlocked(task, 128 + signum);
        return 0;
    }

    uint64_t stack_page = user_rsp & ~(4096ULL - 1ULL);
    if (!vmm_is_mapped((uint64_t *)task->cr3, stack_page)) {
        void *physical = pmm_alloc();
        if (physical == 0 || task->user_page_count >= TASK_MAX_USER_PAGES) {
            if (physical != 0) {
                pmm_free(physical);
            }
            scheduler_terminate_task_unlocked(task, 128 + signum);
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

__attribute__((force_align_arg_pointer))
uint64_t scheduler_tick(uint64_t current_rsp)
{
    kernel_ticks++;
    if (serial_received()) {
        scheduler_wake_stdin_readers();
    }

    uint64_t flags = spin_lock_irqsave(&task_table_lock);

    if (task_count == 0) {
        spin_unlock_irqrestore(&task_table_lock, flags);
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
    int index = current_task_index;

    while (attempts < task_count) {
        index = (index + 1) % (int)task_count;
        attempts++;

        if (index == idle_task_index) {
            continue;
        }

        struct task_control_block *candidate = &tasks[index];
        if (candidate->state == TASK_READY || candidate->state == TASK_RUNNING) {
            
            if (candidate->cr3 != (uint64_t)vmm_kernel_pml4()) {
                if (candidate->pending_signals != 0 && candidate->active_signal == 0) {
                    uint64_t previous_cr3 = (uint64_t)vmm_kernel_pml4();
                    if (current_task != NULL) {
                        previous_cr3 = current_task->cr3;
                    }

                    vmm_switch_address_space((uint64_t *)candidate->cr3);
                    
                    if (!deliver_pending_signal_unlocked(candidate)) {
                        vmm_switch_address_space((uint64_t *)previous_cr3);
                        continue;
                    }
                }
            }

            current_task_index = index;
            current_task = candidate;
            current_task->state = TASK_RUNNING;
            tss_set_rsp0(current_task->kernel_stack_top);
            vmm_switch_address_space((uint64_t *)current_task->cr3);
            
            spin_unlock_irqrestore(&task_table_lock, flags);
            pic_send_eoi();
            return current_task->rsp;
        }
    }

    if (idle_task_index >= 0 && idle_task_index < (int)task_count) {
        struct task_control_block *idle = &tasks[idle_task_index];
        if (idle->state != TASK_ZOMBIE && idle->state != TASK_DEAD && idle->state != TASK_UNUSED) {
            current_task_index = idle_task_index;
            current_task = idle;
            current_task->state = TASK_RUNNING;
            tss_set_rsp0(current_task->kernel_stack_top);
            vmm_switch_address_space((uint64_t *)current_task->cr3);
            spin_unlock_irqrestore(&task_table_lock, flags);
            pic_send_eoi();
            return current_task->rsp;
        }
    }

    spin_unlock_irqrestore(&task_table_lock, flags);
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

int scheduler_fork_current(uint64_t syscall_frame_addr)
{
    task_t *parent = current_task;
    if (parent == 0) {
        return -1;
    }

    struct syscall_frame *sframe = (struct syscall_frame *)syscall_frame_addr;
    if (sframe == 0) {
        return -1;
    }

    uint64_t *child_pml4 = vmm_clone_address_space((uint64_t *)parent->cr3);
    if (child_pml4 == NULL) {
        klog("fork: falha ao clonar PML4\n");
        return -1;
    }

    uint64_t flags = spin_lock_irqsave(&task_table_lock);

    int slot = allocate_task_slot();
    if (slot < 0) {
        spin_unlock_irqrestore(&task_table_lock, flags);
        klog("fork: tabela de tarefas cheia\n");
        vmm_destroy_address_space(child_pml4);
        return -1;
    }

    uint8_t *child_kstack = task_stacks[slot];

    task_t *child = &tasks[slot];
    *child = *parent;

    child->pid          = generate_pid();
    child->parent_pid   = parent->pid;
    child->cr3          = (uint64_t)child_pml4;
    child->state        = TASK_READY;
    child->wait_reason  = TASK_WAIT_NONE;
    child->wait_target  = 0;
    child->exit_status  = 0;
    child->active_signal = 0;
    child->pending_signals = 0;
    child->blocked_signals = parent->blocked_signals;
    child->mutex_wait_next = 0;
    child->user_page_count = parent->user_page_count;
    for (uint32_t i = 0; i < parent->user_page_count; i++) {
        child->user_physical_pages[i] = parent->user_physical_pages[i];
    }
    for (uint32_t i = 0; i < TASK_MAX_FDS; i++) {
        if (child->file_descriptors[i] != 0) {
            child->file_descriptors[i]->ref_count++;
        }
    }
    child->guard_page = 0;
    child->kernel_stack_top = ((uintptr_t)child_kstack + TASK_STACK_SIZE) & ~0xFULL;

    uintptr_t stack_top_addr = ((uintptr_t)child_kstack + TASK_STACK_SIZE) & ~0xFULL;
    struct interrupt_task_frame *child_frame =
        (struct interrupt_task_frame *)(stack_top_addr - sizeof(struct interrupt_task_frame));

    child_frame->ss       = USER_DATA_SELECTOR;
    child_frame->user_rsp = sframe->user_rsp;
    child_frame->rflags   = sframe->r11;
    child_frame->cs       = USER_CODE_SELECTOR;
    child_frame->rip      = sframe->rcx;
    child_frame->rax      = 0;
    child_frame->rbx      = sframe->rbx;
    child_frame->rcx      = sframe->rcx;
    child_frame->rdx      = sframe->rdx;
    child_frame->rsi      = sframe->rsi;
    child_frame->rdi      = sframe->rdi;
    child_frame->rbp      = sframe->rbp;
    child_frame->r8       = sframe->r8;
    child_frame->r9       = sframe->r9;
    child_frame->r10      = sframe->r10;
    child_frame->r11      = sframe->r11;
    child_frame->r12      = sframe->r12;
    child_frame->r13      = sframe->r13;
    child_frame->r14      = sframe->r14;
    child_frame->r15      = sframe->r15;

    child->rsp              = (uint64_t)child_frame;
    child->kernel_stack_top = stack_top_addr;

    int child_pid = (int)child->pid;

    spin_unlock_irqrestore(&task_table_lock, flags);

    klog("fork: filho inserido no escalonador\n");
    return child_pid;
}

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

    if (signum == SIGCHLD || signum == SIGCONT) {
        uintptr_t h = task->signal_handlers[signum];
        if (h == (uintptr_t)0 || h == (uintptr_t)1) {
            task->pending_signals &= ~signal_bit(signum);
            return;
        }
    }

    if (signum == SIGPIPE || signum == SIGINT || signum == SIGTERM) {
        uintptr_t h = task->signal_handlers[signum];
        if (h == (uintptr_t)1) {
            task->pending_signals &= ~signal_bit(signum);
            return;
        }
    }

    uintptr_t handler = signum == SIGKILL ? 0 : task->signal_handlers[signum];
    if (handler == 0 || handler == (uintptr_t)-1) {
        task->pending_signals &= ~signal_bit(signum);
        scheduler_terminate_task(task, 128 + signum);
        return;
    }

    struct syscall_frame *frame = (struct syscall_frame *)syscall_frame_addr;
    uint64_t user_rsp = signal_handler_stack(frame->user_rsp);
    if (user_rsp >= 0x0000800000000000ULL || user_rsp < 0x1000) {
        task->pending_signals &= ~signal_bit(signum);
        scheduler_terminate_task(task, 128 + signum);
        return;
    }

    uint64_t stack_page = user_rsp & ~(4096ULL - 1ULL);
    if (!vmm_is_mapped((uint64_t *)task->cr3, stack_page)) {
        void *physical = pmm_alloc();
        if (physical == 0 || task->user_page_count >= TASK_MAX_USER_PAGES) {
            if (physical != 0) {
                pmm_free(physical);
            }
            scheduler_terminate_task(task, 128 + signum);
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

    frame->rcx = handler;
    frame->user_rsp = user_rsp;
    frame->rdi = (uint64_t)signum;
}
