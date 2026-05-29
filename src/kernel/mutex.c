#include "mutex.h"

#include "scheduler.h"

static uint64_t read_rflags(void)
{
    uint64_t flags;

    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return flags;
}

static int interrupts_are_enabled(void)
{
    return (read_rflags() & (1ULL << 9)) != 0;
}

static int queue_contains(task_t *head, task_t *task)
{
    for (task_t *node = head; node != 0; node = node->mutex_wait_next) {
        if (node == task) {
            return 1;
        }
    }

    return 0;
}

static void queue_push(task_t **head, task_t *task)
{
    task->mutex_wait_next = 0;

    if (*head == 0) {
        *head = task;
        return;
    }

    task_t *tail = *head;
    while (tail->mutex_wait_next != 0) {
        tail = tail->mutex_wait_next;
    }

    tail->mutex_wait_next = task;
}

static task_t *queue_pop(task_t **head)
{
    task_t *task = *head;

    if (task != 0) {
        *head = task->mutex_wait_next;
        task->mutex_wait_next = 0;
    }

    return task;
}

void mutex_init(mutex_t *mutex)
{
    if (mutex == 0) {
        return;
    }

    mutex->locked = 0;
    mutex->pid_owner = 0;
    mutex->wait_queue = 0;
}

void mutex_lock(mutex_t *mutex)
{
    if (mutex == 0) {
        return;
    }

    while (__sync_lock_test_and_set(&mutex->locked, 1)) {
        task_t *current = scheduler_current_task();

        if (current != 0 && interrupts_are_enabled()) {
            if (!queue_contains(mutex->wait_queue, current)) {
                queue_push(&mutex->wait_queue, current);
            }
            current->state = TASK_BLOCKED;
            current->wait_reason = TASK_WAIT_MUTEX;
            current->wait_target = (uint64_t)mutex;
            scheduler_yield();
        } else {
            __asm__ volatile ("pause");
        }
    }

    task_t *current = scheduler_current_task();
    mutex->pid_owner = current != 0 ? (int)current->pid : 0;
}

void mutex_unlock(mutex_t *mutex)
{
    if (mutex == 0) {
        return;
    }

    mutex->pid_owner = 0;
    __sync_lock_release(&mutex->locked);

    task_t *next = queue_pop(&mutex->wait_queue);
    if (next != 0 && next->state == TASK_BLOCKED &&
        next->wait_reason == TASK_WAIT_MUTEX) {
        next->state = TASK_READY;
        next->wait_reason = TASK_WAIT_NONE;
        next->wait_target = 0;
    }
}
