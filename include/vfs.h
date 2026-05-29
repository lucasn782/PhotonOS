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
} vfs_node_type_t;

typedef struct vfs_node vfs_node_t;

typedef size_t (*vfs_read_t)(vfs_node_t *node, size_t offset, size_t size,
    uint8_t *buffer);
typedef size_t (*vfs_write_t)(vfs_node_t *node, size_t offset, size_t size,
    const uint8_t *buffer);
typedef int (*vfs_open_t)(vfs_node_t *node);
typedef void (*vfs_close_t)(vfs_node_t *node);

struct vfs_node {
    char name[VFS_NAME_MAX];
    vfs_node_type_t type;
    size_t size;
    void *data;
    vfs_read_t read;
    vfs_write_t write;
    vfs_open_t open;
    vfs_close_t close;
    vfs_node_t *parent;
    vfs_node_t *child;
    vfs_node_t *sibling;
};

void vfs_init(void);
vfs_node_t *vfs_root(void);
vfs_node_t *vfs_create_node(vfs_node_t *parent, const char *name,
    vfs_node_type_t type);
vfs_node_t *vfs_find(const char *path);
size_t vfs_read(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer);
size_t vfs_write(vfs_node_t *node, size_t offset, size_t size,
    const uint8_t *buffer);

#endif
