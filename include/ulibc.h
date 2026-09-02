#ifndef PHOTONOS_ULIBC_H
#define PHOTONOS_ULIBC_H

#include <stddef.h>
#include <stdint.h>

#include "proc.h"
#include "string.h"
#include "stdio.h"

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

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define WNOHANG 1

typedef uint32_t sigset_t;

struct sigaction {
    sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
};

#define VFS_NAME_MAX 64

typedef struct vfs_dir_entry {
    char name[VFS_NAME_MAX];
    uint32_t type;
    uint32_t size;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t nlink;
} vfs_dir_entry_t;

// Local Heap memory allocations
void *malloc(size_t size);
void free(void *ptr);

#include "sys/socket.h"
#include "sys/stat.h"

// Core system wrappers
int open(const char *path, int flags);
int read(int fd, void *buf, int count);
int write(int fd, const void *buf, int count);
int close(int fd);
int fork(void);
void exit(int status);
void yield(void);
int wait(int *status);
int waitpid(int pid, int *status, int options);
int pipe(int fds[2]);

sighandler_t signal(int signum, sighandler_t handler);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int kill(int pid, int signum);
void sigreturn(void);
int getprocs(proc_info_t *buffer, size_t max_size);
uint64_t get_ticks(void);
int readdir(int fd, vfs_dir_entry_t *buf, uint32_t count);
int lseek(int fd, long offset, int whence);
int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mkdir(const char *path, uint32_t mode);
int rmdir(const char *path);
int chmod(const char *path, uint32_t mode);
int chown(const char *path, uint32_t uid, uint32_t gid);
int link(const char *oldpath, const char *newpath);
int unlink(const char *pathname);
int symlink(const char *target, const char *linkpath);
int readlink(const char *pathname, char *buf, size_t bufsiz);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int truncate(const char *path, size_t length);
int ftruncate(int fd, size_t length);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int sync(void);
uint32_t umask(uint32_t mask);
int flock(int fd, int op);
int fcntl(int fd, int cmd, ...);
int mount(const char *source, const char *target, const char *fs_type, uint64_t flags);
int umount(const char *target);

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4

#endif
