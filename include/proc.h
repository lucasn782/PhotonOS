#ifndef PHOTONOS_PROC_H
#define PHOTONOS_PROC_H

#include <stdint.h>

#define PROC_NAME_MAX 32

#define PROC_FLAG_FOREGROUND 0x1U

enum proc_state {
    PROC_STATE_READY = 1,
    PROC_STATE_RUNNING,
    PROC_STATE_SLEEPING,
    PROC_STATE_WAITING,
    PROC_STATE_BLOCKED,
    PROC_STATE_ZOMBIE,
    PROC_STATE_STOPPED,
};

typedef struct proc_info {
    uint32_t pid;
    uint32_t state;
    uint32_t flags;
    char name[PROC_NAME_MAX];
} proc_info_t;

#endif
