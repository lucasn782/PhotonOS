#ifndef PHOTONOS_TASK_H
#define PHOTONOS_TASK_H

#include <stdint.h>

#define TASK_MAX_FDS 32
#define TASK_MAX_USER_PAGES 128
#define TASK_SIGNAL_COUNT 32
#define TASK_NAME_MAX 32

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20

#define SIG_DFL ((uintptr_t)0)
#define SIG_IGN ((uintptr_t)1)

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define WNOHANG 1

#define SIGNAL_TRAMPOLINE_ADDR 0x0000008000700000ULL

typedef uint32_t sigset_t;
typedef void (*sighandler_t)(int);

struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
};

typedef struct vfs_node vfs_node_t;

typedef struct file_description {
    vfs_node_t *node;
    uint64_t offset;
    uint32_t flags;
    uint32_t ref_count;
} file_description_t;

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_WAITING,
    TASK_BLOCKED,
    TASK_STOPPED,
    TASK_ZOMBIE,
    TASK_DEAD,
};

enum task_wait_reason {
    TASK_WAIT_NONE,
    TASK_WAIT_STDIN,
    TASK_WAIT_CHILD,
    TASK_WAIT_PIPE_READ,
    TASK_WAIT_PIPE_WRITE,
    TASK_WAIT_MUTEX,
    TASK_WAIT_SOCKET_RECV,
    TASK_WAIT_NETWORK,
};

struct task_registers {
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
    uint64_t rflags;
};

struct task_signal_context {
    struct task_registers registers;
    uint64_t user_rsp;
    uint64_t cs;
    uint64_t ss;
    uint32_t signum;
};

struct task_sigreturn_frame {
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

struct task_control_block {
    uint64_t rsp;
    uint64_t kernel_stack_top;
    uintptr_t guard_page;
    uint64_t cr3;
    file_description_t *file_descriptors[TASK_MAX_FDS];
    char cwd[64];
    uint64_t user_physical_pages[TASK_MAX_USER_PAGES];
    uintptr_t heap_start;
    uintptr_t heap_end;
    uint32_t user_page_count;
    struct task_registers registers;
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t uid;
    uint32_t gid;
    uint32_t umask;
    char name[TASK_NAME_MAX];
    uintptr_t signal_handlers[TASK_SIGNAL_COUNT];
    uint32_t pending_signals;
    uint32_t blocked_signals;
    uint32_t active_signal;
    struct task_signal_context signal_context;
    struct task_sigreturn_frame sigreturn_frame;
    uint64_t wait_target;
    int exit_status;
    enum task_state state;
    enum task_wait_reason wait_reason;
    struct task_control_block *mutex_wait_next;
};

typedef struct task_control_block task_t;

file_description_t *file_description_alloc(vfs_node_t *node, uint32_t flags);
void file_description_unref(file_description_t *fdesc);
void task_close_all_fds(task_t *task);
void switch_to(uint64_t *current_rsp, uint64_t next_rsp);

#endif

