#ifndef PHOTONOS_SCHEDULER_H
#define PHOTONOS_SCHEDULER_H

#include <stdint.h>

#include "task.h"

typedef void (*task_entry_t)(void);

void scheduler_init(void);
int scheduler_create_task(task_entry_t entry);
int scheduler_create_user_task(task_entry_t entry);
int scheduler_add_user_process(uint64_t entry, uint64_t user_rsp,
    uint64_t *pml4, task_t *out_task, uint64_t arg_rdi, uint64_t arg_rsi);
task_t *scheduler_current_task(void);
task_t *scheduler_find_task(uint32_t pid);
void scheduler_task_table_lock(void);
void scheduler_task_table_unlock(void);
uint32_t scheduler_task_count(void);
task_t *scheduler_task_at(uint32_t index);
void scheduler_yield(void);
int scheduler_send_signal(uint32_t pid, int signum);
void scheduler_sleep_current(enum task_wait_reason reason, uint64_t target);
void scheduler_wake_stdin_readers(void);
void scheduler_wake_pipe_readers(void *pipe);
void scheduler_wake_pipe_writers(void *pipe);
void scheduler_wake_socket_receivers(uint8_t protocol);
int scheduler_wait_current(uint32_t pid);
void scheduler_terminate_task(task_t *task, int status);
void scheduler_exit_current(int status);
int scheduler_fork_current(uint64_t syscall_frame_addr);
void scheduler_handle_syscall_signals(uint64_t syscall_frame_addr, uint64_t syscall_ret);
uint64_t scheduler_tick(uint64_t current_rsp);
int task_alloc_fd(task_t *task, vfs_node_t *node);
void scheduler_wake_socket(void *sock);

#endif
