# Virtual File System (VFS)

This document describes the Virtual File System (VFS) layer of PhotonOS, directory traversing, unified file nodes structure, and mount operations.

---

## 1. VFS Abstract Layer

The Virtual File System (VFS) decouples kernel and userspace requests from storage implementations. Every file, directory, pipe, and device is represented as a **VFS Node** (`vfs_node_t` or `struct vfs_node` in `include/vfs.h`):

```c
struct vfs_node {
    char name[128];                  // Node name string
    uint32_t flags;                  // File type: regular, directory, device, pipe, etc.
    uint32_t size;                   // Total size of the file in bytes
    uint32_t inode;                  // Low-level filesystem identifier (inode index)
    void *data;                      // Private pointer for implementation data (e.g. ext2_node_data)
    
    // Unified operations pointers
    int (*read)(struct vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer);
    int (*write)(struct vfs_node *node, uint64_t offset, uint64_t size, const uint8_t *buffer);
    int (*open)(struct vfs_node *node);
    int (*close)(struct vfs_node *node);
    int (*readdir)(struct vfs_node *node, uint32_t index, struct vfs_dir_entry *entry);
    struct vfs_node *(*find)(struct vfs_node *node, const char *name);
};
```

---

## 2. Directory Traversing and Mount points

- **Root Directory (`/`)**: A root node is initialized at boot. Filesystems are mounted at specific path targets via `vfs_mount()`.
- **Recursion Lookup (`vfs_find`)**: When an application accesses `/usr/bin/hello`, the VFS traverses the tree:
  1. Tokenizes the path by `/`.
  2. Queries the current directory node using its `find` function pointer.
  3. Repeats recursively until the target node is resolved or a traversal error is hit.

---

## 3. Standard Devices Registration

The VFS mounts default device nodes at boot-time:
- **`stdin`** (FD 0): Mapped to PS/2 Keyboard input.
- **`stdout`** (FD 1): Mapped to serial console COM1 or graphic video buffer outputs.
- **`stderr`** (FD 2): Mapped similarly.
- Sockets and Pipes also register temporary VFS nodes of type `VFS_NODE_DEVICE` and `VFS_NODE_PIPE` respectively.
