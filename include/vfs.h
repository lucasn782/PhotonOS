#ifndef PHOTONOS_VFS_H
#define PHOTONOS_VFS_H

#include <stddef.h>
#include <stdint.h>

#define VFS_NAME_MAX 64

#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

typedef enum vfs_node_type {
    VFS_NODE_FILE,
    VFS_NODE_DIRECTORY,
    VFS_NODE_DEVICE,
    VFS_NODE_PIPE,
    VFS_NODE_SOCKET,
    VFS_NODE_SYMLINK,
} vfs_node_type_t;

struct mutex;
extern struct mutex vfs_mutex;

typedef struct vfs_node vfs_node_t;

typedef struct vfs_dir_entry {
    char name[VFS_NAME_MAX];
    uint32_t type;
    uint32_t size;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t nlink;
} vfs_dir_entry_t;

typedef int (*vfs_read_t)(vfs_node_t *node, uint64_t offset, uint32_t size,
    uint8_t *buffer);
typedef size_t (*vfs_write_t)(vfs_node_t *node, size_t offset, size_t size,
    const uint8_t *buffer);
typedef int (*vfs_open_t)(vfs_node_t *node);
typedef void (*vfs_close_t)(vfs_node_t *node);
typedef int (*vfs_readdir_t)(vfs_node_t *node, uint32_t index, vfs_dir_entry_t *entry);
typedef int (*vfs_truncate_t)(vfs_node_t *node, size_t length);

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

struct vfs_node {
    char name[VFS_NAME_MAX];
    vfs_node_type_t type;
    size_t size;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint32_t nlink;
    uint32_t ref_count;
    uint32_t lock_type;
    uint32_t lock_count;
    uint32_t lock_owner_pid;
    char symlink_target[VFS_NAME_MAX];
    void *data;
    vfs_read_t read;
    vfs_write_t write;
    vfs_open_t open;
    vfs_close_t close;
    vfs_readdir_t readdir;
    vfs_truncate_t truncate;
    vfs_node_t *parent;
    vfs_node_t *child;
    vfs_node_t *sibling;
    vfs_node_t *mounted_here;
};

typedef struct vfs_mount {
    char mount_point[VFS_NAME_MAX];
    char source[VFS_NAME_MAX];
    char fs_type[32];
    vfs_node_t *root_node;
    vfs_node_t *mount_over;
    struct vfs_mount *next;
} vfs_mount_t;

extern vfs_mount_t *vfs_mount_list;

void vfs_init(void);
vfs_node_t *vfs_root(void);
vfs_node_t *vfs_create_node(vfs_node_t *parent, const char *name,
    vfs_node_type_t type);
void vfs_node_ref(vfs_node_t *node);
void vfs_node_unref(vfs_node_t *node);
vfs_node_t *vfs_find(const char *path);
vfs_node_t *vfs_find_relative(vfs_node_t *base, const char *path);
vfs_node_t *vfs_find_following_symlinks(const char *path, int max_depth);
int vfs_build_path(const char *base_cwd, const char *input_path, char *out_buf, size_t out_capacity);
int vfs_read(vfs_node_t *node, uint64_t offset, uint32_t size, uint8_t *buffer);
size_t vfs_write(vfs_node_t *node, size_t offset, size_t size,
    const uint8_t *buffer);
int vfs_readdir(vfs_node_t *node, uint32_t index, vfs_dir_entry_t *entry);
int vfs_truncate(vfs_node_t *node, size_t length);
int vfs_truncate_path(const char *path, size_t length);

int vfs_chdir(const char *path);
int vfs_getcwd(char *buffer, size_t size);

int vfs_check_permission(vfs_node_t *node, uint32_t mask, uint32_t uid, uint32_t gid);
int vfs_chmod(const char *path, uint32_t mode);
int vfs_chown(const char *path, uint32_t uid, uint32_t gid);
int vfs_link(const char *oldpath, const char *newpath);
int vfs_unlink(const char *pathname);
int vfs_mkdir(const char *path, uint32_t mode);
int vfs_rmdir(const char *path);
int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *pathname, char *buf, size_t bufsiz);
int vfs_flock(vfs_node_t *node, int op, uint32_t pid);
void vfs_node_unlock_by_pid(vfs_node_t *node, uint32_t pid);
int vfs_sync(void);

int vfs_mount(const char *source, const char *target, const char *fs_type, uint64_t flags);
int vfs_umount(const char *target);

#endif
