#include "vfs.h"

#include "heap.h"
#include "mutex.h"

static vfs_node_t *root_node;
mutex_t vfs_mutex;
vfs_mount_t *vfs_mount_list = 0;

static void memory_zero(void *ptr, size_t size)
{
    uint8_t *bytes = ptr;

    for (size_t i = 0; i < size; i++) {
        bytes[i] = 0;
    }
}

static int string_equals_n(const char *left, const char *right, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (left[i] != right[i] || right[i] == '\0') {
            return 0;
        }
    }

    return right[length] == '\0';
}

static int string_equals(const char *s1, const char *s2)
{
    if (s1 == 0 || s2 == 0) return 0;
    size_t i = 0;
    while (s1[i] != '\0' && s2[i] != '\0') {
        if (s1[i] != s2[i]) return 0;
        i++;
    }
    return s1[i] == s2[i];
}

static void copy_name(char *dest, const char *src)
{
    size_t i = 0;

    while (src[i] != '\0' && i < VFS_NAME_MAX - 1) {
        dest[i] = src[i];
        i++;
    }

    dest[i] = '\0';
}

static vfs_node_t *find_child(vfs_node_t *parent, const char *name,
    size_t length)
{
    for (vfs_node_t *node = parent->child; node != 0; node = node->sibling) {
        if (string_equals_n(name, node->name, length)) {
            if (node->mounted_here != 0) {
                return node->mounted_here;
            }
            return node;
        }
    }

    return 0;
}

void vfs_init(void)
{
    mutex_init(&vfs_mutex);
    vfs_mount_list = 0;
    root_node = vfs_create_node(0, "/", VFS_NODE_DIRECTORY);
}

vfs_node_t *vfs_root(void)
{
    return root_node;
}

vfs_node_t *vfs_create_node(vfs_node_t *parent, const char *name,
    vfs_node_type_t type)
{
    vfs_node_t *node = kmalloc(sizeof(*node));
    if (node == 0) {
        return 0;
    }

    memory_zero(node, sizeof(*node));
    copy_name(node->name, name);
    node->type = type;
    node->parent = parent;
    node->uid = 0;
    node->gid = 0;
    node->mode = (type == VFS_NODE_DIRECTORY) ? 0755 : 0644;
    node->nlink = 1;
    node->symlink_target[0] = '\0';
    node->mounted_here = 0;

    mutex_lock(&vfs_mutex);
    if (parent != 0) {
        node->sibling = parent->child;
        parent->child = node;
    }
    mutex_unlock(&vfs_mutex);

    return node;
}

vfs_node_t *vfs_find(const char *path)
{
    if (root_node == 0 || path == 0 || path[0] == '\0') {
        return 0;
    }

    vfs_node_t *current = root_node;
    size_t index = 0;

    if (path[index] == '/') {
        index++;
    }

    mutex_lock(&vfs_mutex);
    while (path[index] != '\0') {
        while (path[index] == '/') {
            index++;
        }

        if (path[index] == '\0') {
            break;
        }

        size_t start = index;
        while (path[index] != '\0' && path[index] != '/') {
            index++;
        }

        current = find_child(current, path + start, index - start);
        if (current == 0) {
            mutex_unlock(&vfs_mutex);
            return 0;
        }
    }

    mutex_unlock(&vfs_mutex);
    return current;
}

vfs_node_t *vfs_find_following_symlinks(const char *path, int max_depth)
{
    if (max_depth <= 0) {
        return 0;
    }

    vfs_node_t *node = vfs_find(path);
    if (node == 0) {
        return 0;
    }

    if (node->type == VFS_NODE_SYMLINK) {
        if (node->symlink_target[0] != '\0') {
            return vfs_find_following_symlinks(node->symlink_target, max_depth - 1);
        }
    }

    return node;
}

int vfs_check_permission(vfs_node_t *node, uint32_t mask, uint32_t uid, uint32_t gid)
{
    if (node == 0) return 0;
    if (uid == 0) return 1; /* Root Superuser has full access */

    uint32_t mode = node->mode;
    if (node->uid == uid) {
        return ((mode >> 6) & mask) == mask;
    } else if (node->gid == gid) {
        return ((mode >> 3) & mask) == mask;
    }

    return (mode & mask) == mask;
}

int vfs_chmod(const char *path, uint32_t mode)
{
    vfs_node_t *node = vfs_find(path);
    if (node == 0) return -1;

    mutex_lock(&vfs_mutex);
    node->mode = mode & 0777;
    mutex_unlock(&vfs_mutex);
    return 0;
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    vfs_node_t *node = vfs_find(path);
    if (node == 0) return -1;

    mutex_lock(&vfs_mutex);
    node->uid = uid;
    node->gid = gid;
    mutex_unlock(&vfs_mutex);
    return 0;
}

int vfs_link(const char *oldpath, const char *newpath)
{
    vfs_node_t *old_node = vfs_find(oldpath);
    if (old_node == 0 || old_node->type == VFS_NODE_DIRECTORY) {
        return -1;
    }

    /* Extract directory and new node name from newpath */
    char parent_path[VFS_NAME_MAX];
    char new_name[VFS_NAME_MAX];
    size_t last_slash = 0;
    size_t i = 0;

    while (newpath[i] != '\0' && i < VFS_NAME_MAX - 1) {
        if (newpath[i] == '/') last_slash = i;
        i++;
    }

    if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        copy_name(new_name, newpath[0] == '/' ? newpath + 1 : newpath);
    } else {
        size_t p = 0;
        for (; p < last_slash; p++) parent_path[p] = newpath[p];
        parent_path[p] = '\0';
        copy_name(new_name, newpath + last_slash + 1);
    }

    vfs_node_t *parent = vfs_find(parent_path);
    if (parent == 0 || parent->type != VFS_NODE_DIRECTORY) {
        return -1;
    }

    vfs_node_t *new_node = vfs_create_node(parent, new_name, old_node->type);
    if (new_node == 0) {
        return -1;
    }

    mutex_lock(&vfs_mutex);
    new_node->data = old_node->data;
    new_node->read = old_node->read;
    new_node->write = old_node->write;
    new_node->open = old_node->open;
    new_node->close = old_node->close;
    new_node->readdir = old_node->readdir;
    new_node->size = old_node->size;
    new_node->uid = old_node->uid;
    new_node->gid = old_node->gid;
    new_node->mode = old_node->mode;

    old_node->nlink++;
    new_node->nlink = old_node->nlink;
    mutex_unlock(&vfs_mutex);

    return 0;
}

int vfs_unlink(const char *pathname)
{
    vfs_node_t *node = vfs_find(pathname);
    if (node == 0) {
        return -1;
    }

    mutex_lock(&vfs_mutex);
    if (node->nlink > 0) {
        node->nlink--;
    }

    if (node->parent != 0) {
        vfs_node_t *prev = 0;
        vfs_node_t *curr = node->parent->child;
        while (curr != 0) {
            if (curr == node) {
                if (prev != 0) {
                    prev->sibling = curr->sibling;
                } else {
                    node->parent->child = curr->sibling;
                }
                break;
            }
            prev = curr;
            curr = curr->sibling;
        }
    }

    if (node->nlink == 0) {
        kfree(node);
    }
    mutex_unlock(&vfs_mutex);

    return 0;
}

int vfs_symlink(const char *target, const char *linkpath)
{
    if (target == 0 || linkpath == 0) return -1;

    char parent_path[VFS_NAME_MAX];
    char link_name[VFS_NAME_MAX];
    size_t last_slash = 0;
    size_t i = 0;

    while (linkpath[i] != '\0' && i < VFS_NAME_MAX - 1) {
        if (linkpath[i] == '/') last_slash = i;
        i++;
    }

    if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        copy_name(link_name, linkpath[0] == '/' ? linkpath + 1 : linkpath);
    } else {
        size_t p = 0;
        for (; p < last_slash; p++) parent_path[p] = linkpath[p];
        parent_path[p] = '\0';
        copy_name(link_name, linkpath + last_slash + 1);
    }

    vfs_node_t *parent = vfs_find(parent_path);
    if (parent == 0 || parent->type != VFS_NODE_DIRECTORY) {
        return -1;
    }

    vfs_node_t *node = vfs_create_node(parent, link_name, VFS_NODE_SYMLINK);
    if (node == 0) return -1;

    mutex_lock(&vfs_mutex);
    copy_name(node->symlink_target, target);
    node->size = i;
    mutex_unlock(&vfs_mutex);

    return 0;
}

int vfs_readlink(const char *pathname, char *buf, size_t bufsiz)
{
    vfs_node_t *node = vfs_find(pathname);
    if (node == 0 || node->type != VFS_NODE_SYMLINK || buf == 0 || bufsiz == 0) {
        return -1;
    }

    mutex_lock(&vfs_mutex);
    size_t i = 0;
    while (node->symlink_target[i] != '\0' && i < bufsiz - 1) {
        buf[i] = node->symlink_target[i];
        i++;
    }
    buf[i] = '\0';
    mutex_unlock(&vfs_mutex);

    return (int)i;
}

int vfs_mount(const char *source, const char *target, const char *fs_type, uint64_t flags)
{
    (void)flags;
    vfs_node_t *target_node = vfs_find(target);
    if (target_node == 0 || target_node->type != VFS_NODE_DIRECTORY) {
        return -1;
    }

    vfs_mount_t *mnt = kmalloc(sizeof(*mnt));
    if (mnt == 0) return -1;

    memory_zero(mnt, sizeof(*mnt));
    copy_name(mnt->source, source != 0 ? source : "none");
    copy_name(mnt->mount_point, target);
    copy_name(mnt->fs_type, fs_type != 0 ? fs_type : "generic");

    vfs_node_t *fs_root = vfs_create_node(0, target_node->name, VFS_NODE_DIRECTORY);
    mnt->root_node = fs_root;
    mnt->mount_over = target_node;

    mutex_lock(&vfs_mutex);
    target_node->mounted_here = fs_root;
    mnt->next = vfs_mount_list;
    vfs_mount_list = mnt;
    mutex_unlock(&vfs_mutex);

    return 0;
}

int vfs_umount(const char *target)
{
    mutex_lock(&vfs_mutex);
    vfs_mount_t *prev = 0;
    vfs_mount_t *curr = vfs_mount_list;

    while (curr != 0) {
        if (string_equals(curr->mount_point, target)) {
            if (curr->mount_over != 0) {
                curr->mount_over->mounted_here = 0;
            }
            if (prev != 0) {
                prev->next = curr->next;
            } else {
                vfs_mount_list = curr->next;
            }
            kfree(curr);
            mutex_unlock(&vfs_mutex);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }

    mutex_unlock(&vfs_mutex);
    return -1;
}

int vfs_read(vfs_node_t *node, uint64_t offset, uint32_t size, uint8_t *buffer)
{
    if (node == 0 || node->read == 0 || buffer == 0) {
        return 0;
    }

    mutex_lock(&vfs_mutex);
    int bytes = node->read(node, offset, size, buffer);
    mutex_unlock(&vfs_mutex);

    return bytes;
}

size_t vfs_write(vfs_node_t *node, size_t offset, size_t size,
    const uint8_t *buffer)
{
    if (node == 0 || node->write == 0 || buffer == 0) {
        return 0;
    }

    mutex_lock(&vfs_mutex);
    size_t bytes = node->write(node, offset, size, buffer);
    mutex_unlock(&vfs_mutex);

    return bytes;
}

int vfs_readdir(vfs_node_t *node, uint32_t index, vfs_dir_entry_t *entry)
{
    if (node == 0 || node->type != VFS_NODE_DIRECTORY || entry == 0) {
        return -1;
    }

    mutex_lock(&vfs_mutex);

    if (node->readdir != 0) {
        int result = node->readdir(node, index, entry);
        mutex_unlock(&vfs_mutex);
        return result;
    }

    vfs_node_t *child = node->child;
    for (uint32_t i = 0; i < index && child != 0; i++) {
        child = child->sibling;
    }

    if (child == 0) {
        mutex_unlock(&vfs_mutex);
        return 0;
    }

    memory_zero(entry, sizeof(*entry));
    copy_name(entry->name, child->name);
    entry->size = (uint32_t)child->size;
    entry->type = (uint32_t)child->type;
    entry->uid = child->uid;
    entry->gid = child->gid;
    entry->mode = child->mode;
    entry->nlink = child->nlink;

    mutex_unlock(&vfs_mutex);
    return 1;
}
