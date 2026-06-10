#ifndef PHOTONOS_ULIBC_H
#define PHOTONOS_ULIBC_H

#include <stddef.h>

#include "proc.h"

#define SIGINT 2
#define SIGKILL 9
#define SIGTERM 15

typedef void (*sighandler_t)(int);

#define VFS_NAME_MAX 64

typedef struct vfs_dir_entry {
    char name[VFS_NAME_MAX];
    uint32_t type;
    uint32_t size;
} vfs_dir_entry_t;

void *malloc(size_t size);
void free(void *ptr);
size_t strlen(const char *str);
void *memcpy(void *dest, const void *src, size_t size);
int printf(const char *format, ...);
void exit(int status);
void yield(void);
sighandler_t signal(int signum, sighandler_t handler);
int kill(int pid, int signum);
void sigreturn(void);
int getprocs(proc_info_t *buffer, size_t max_size);
uint64_t get_ticks(void);
int readdir(int fd, vfs_dir_entry_t *buf, uint32_t count);

#endif
