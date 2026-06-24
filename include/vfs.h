#ifndef PHOTONOS_VFS_H
#define PHOTONOS_VFS_H

#include <stddef.h>
#include <stdint.h>

#define VFS_NAME_MAX 64

typedef enum vfs_node_type {
    VFS_NODE_FILE,
    VFS_NODE_DIRECTORY,
    VFS_NODE_DEVICE,
    VFS_NODE_PIPE,
    VFS_NODE_SOCKET,
} vfs_node_type_t;

struct mutex;
extern struct mutex vfs_mutex;

typedef struct vfs_node vfs_node_t;

typedef struct vfs_dir_entry {
    char name[VFS_NAME_MAX];
    uint32_t type;
    uint32_t size;
} vfs_dir_entry_t;

typedef int (*vfs_read_t)(vfs_node_t *node, uint64_t offset, uint32_t size,
    uint8_t *buffer);
typedef size_t (*vfs_write_t)(vfs_node_t *node, size_t offset, size_t size,
    const uint8_t *buffer);
typedef int (*vfs_open_t)(vfs_node_t *node);
typedef void (*vfs_close_t)(vfs_node_t *node);
typedef int (*vfs_readdir_t)(vfs_node_t *node, uint32_t index, vfs_dir_entry_t *entry);

struct vfs_node {
    char name[VFS_NAME_MAX];
    vfs_node_type_t type;
    size_t size;
    void *data;
    vfs_read_t read;
    vfs_write_t write;
    vfs_open_t open;
    vfs_close_t close;
    vfs_readdir_t readdir;
    vfs_node_t *parent;
    vfs_node_t *child;
    vfs_node_t *sibling;
};

void vfs_init(void);
vfs_node_t *vfs_root(void);
vfs_node_t *vfs_create_node(vfs_node_t *parent, const char *name,
    vfs_node_type_t type);
vfs_node_t *vfs_find(const char *path);
int vfs_read(vfs_node_t *node, uint64_t offset, uint32_t size, uint8_t *buffer);
size_t vfs_write(vfs_node_t *node, size_t offset, size_t size,
    const uint8_t *buffer);
int vfs_readdir(vfs_node_t *node, uint32_t index, vfs_dir_entry_t *entry);

#endif
