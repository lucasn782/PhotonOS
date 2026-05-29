#ifndef PHOTONOS_ULIBC_H
#define PHOTONOS_ULIBC_H

#include <stddef.h>

#include "proc.h"

#define SIGINT 2
#define SIGKILL 9
#define SIGTERM 15

typedef void (*sighandler_t)(int);

void *malloc(size_t size);
void free(void *ptr);
size_t strlen(const char *str);
void *memcpy(void *dest, const void *src, size_t size);
sighandler_t signal(int signum, sighandler_t handler);
int kill(int pid, int signum);
void sigreturn(void);
int getprocs(proc_info_t *buffer, size_t max_size);

#endif
