#ifndef PHOTONOS_MUTEX_H
#define PHOTONOS_MUTEX_H

#include <stdint.h>

#include "task.h"

typedef struct mutex {
    volatile int locked;
    int pid_owner;
    task_t *wait_queue;
} mutex_t;

void mutex_init(mutex_t *mutex);
void mutex_lock(mutex_t *mutex);
void mutex_unlock(mutex_t *mutex);

#endif
