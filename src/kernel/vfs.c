#include "vfs.h"

#include "bcache.h"
#include "heap.h"
#include "mutex.h"
#include "fat16.h"
#include "fs/ext2.h"
#include "scheduler.h"

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

static size_t string_length(const char *str)
{
    if (str == 0) return 0;
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    return len;
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
    task_t *task = scheduler_current_task();
    uint32_t umask = task ? task->umask : 0022;
    node->uid = task ? task->uid : 0;
    node->gid = task ? task->gid : 0;
    node->mode = (type == VFS_NODE_DIRECTORY) ? ((0777 & ~umask) & 0777) : ((0666 & ~umask) & 0777);
    node->nlink = 1;
    node->ref_count = 0;
    node->lock_type = 0;
    node->lock_count = 0;
    node->lock_owner_pid = 0;
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

void vfs_node_ref(vfs_node_t *node)
{
    if (node == 0) return;
    mutex_lock(&vfs_mutex);
    node->ref_count++;
    mutex_unlock(&vfs_mutex);
}

void vfs_node_unref(vfs_node_t *node)
{
    if (node == 0) return;
    mutex_lock(&vfs_mutex);
    if (node->ref_count > 0) {
        node->ref_count--;
    }
    if (node->ref_count == 0 && node->parent == 0 && node != root_node) {
        if (node->data != 0) {
            kfree(node->data);
            node->data = 0;
        }
        kfree(node);
    }
    mutex_unlock(&vfs_mutex);
}

vfs_node_t *vfs_find_relative(vfs_node_t *base, const char *path)
{
    if (root_node == 0 || path == 0 || path[0] == '\0') {
        return 0;
    }

    vfs_node_t *current = base;
    if (path[0] == '/' || current == 0) {
        current = root_node;
    }

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

        size_t len = index - start;

        if (current->type != VFS_NODE_DIRECTORY) {
            mutex_unlock(&vfs_mutex);
            return 0;
        }

        if (len == 1 && path[start] == '.') {
            continue;
        }

        if (len == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (current->parent != 0) {
                current = current->parent;
            }
            continue;
        }

        current = find_child(current, path + start, len);
        if (current == 0) {
            mutex_unlock(&vfs_mutex);
            return 0;
        }
    }

    mutex_unlock(&vfs_mutex);
    return current;
}

vfs_node_t *vfs_find(const char *path)
{
    return vfs_find_relative(root_node, path);
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
    if (pathname == 0 || pathname[0] == '\0') {
        return -1;
    }

    if (fat16_is_mounted() && fat16_vfs_unlink(pathname) != 0) {
        return -1;
    }

    vfs_node_t *node = vfs_find(pathname);
    if (node == 0 || node->type == VFS_NODE_DIRECTORY) {
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
        node->parent = 0;
        node->sibling = 0;
    }

    if (node->ref_count == 0 && node->nlink == 0) {
        if (node->data != 0) {
            kfree(node->data);
            node->data = 0;
        }
        kfree(node);
    }
    mutex_unlock(&vfs_mutex);

    return 0;
}

int vfs_mkdir(const char *path, uint32_t mode)
{
    if (path == 0 || path[0] == '\0') {
        return -1;
    }

    /*
     * The active root filesystem owns directory creation.  Falling back to
     * an in-memory node after a FAT16 failure makes the VFS tree disagree
     * with the disk (for example, when the directory table is full).
     */
    if (fat16_is_mounted()) {
        return fat16_vfs_mkdir(path, mode);
    }

    char parent_path[VFS_NAME_MAX];
    char dir_name[VFS_NAME_MAX];
    size_t last_slash = 0;
    size_t i = 0;

    while (path[i] != '\0' && i < VFS_NAME_MAX - 1) {
        if (path[i] == '/') last_slash = i;
        i++;
    }

    if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
        copy_name(dir_name, path[0] == '/' ? path + 1 : path);
    } else {
        size_t p = 0;
        for (; p < last_slash; p++) parent_path[p] = path[p];
        parent_path[p] = '\0';
        copy_name(dir_name, path + last_slash + 1);
    }

    if (dir_name[0] == '\0') {
        return -1;
    }

    vfs_node_t *parent = vfs_find(parent_path);
    if (parent == 0 || parent->type != VFS_NODE_DIRECTORY) {
        return -1;
    }

    if (find_child(parent, dir_name, string_length(dir_name)) != 0) {
        return -1;
    }

    vfs_node_t *node = vfs_create_node(parent, dir_name, VFS_NODE_DIRECTORY);
    if (node == 0) {
        return -1;
    }

    task_t *task = scheduler_current_task();
    uint32_t umask = task ? task->umask : 0022;
    node->mode = mode ? ((mode & ~umask) & 0777) : ((0777 & ~umask) & 0777);
    return 0;
}

int vfs_rmdir(const char *path)
{
    if (path == 0 || path[0] == '\0') {
        return -1;
    }

    /*
     * Do not remove the cached VFS node unless the persistent removal has
     * succeeded.  Previously a failed FAT16 rmdir (notably for a non-empty
     * directory) was ignored and only the in-memory node was removed.
     */
    if (fat16_is_mounted() && fat16_vfs_rmdir(path) != 0) {
        return -1;
    }

    vfs_node_t *node = vfs_find(path);
    if (node == 0 || node->type != VFS_NODE_DIRECTORY) {
        return -1;
    }

    if (node == vfs_root()) {
        return -1;
    }

    mutex_lock(&vfs_mutex);
    if (node->child != 0) {
        mutex_unlock(&vfs_mutex);
        return -1;
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
        node->parent = 0;
        node->sibling = 0;
    }

    if (node->ref_count == 0) {
        if (node->data != 0) {
            kfree(node->data);
            node->data = 0;
        }
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
    if (fs_root == 0) {
        kfree(mnt);
        return -1;
    }
    mnt->root_node = fs_root;
    mnt->mount_over = target_node;

    if (fs_type != 0 && string_equals(fs_type, "ext2")) {
        uint32_t part_lba = 0;
        if (source != 0 && string_equals(source, "/dev/ata0p2")) {
            part_lba = 2048;
        }
        if (!ext2_mount_at(fs_root, part_lba)) {
            kfree(fs_root);
            kfree(mnt);
            return -1;
        }
    }

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

int vfs_build_path(const char *base_cwd, const char *input_path, char *out_buf, size_t out_capacity)
{
    if (input_path == 0 || out_buf == 0 || out_capacity < 2) {
        return -1;
    }

    const char *comp_ptrs[32];
    size_t comp_lens[32];
    int ncomps = 0;

    /* If input_path is relative, start with base_cwd components */
    if (input_path[0] != '/') {
        const char *cwd = (base_cwd != 0 && base_cwd[0] != '\0') ? base_cwd : "/";
        size_t ci = 0;
        while (cwd[ci] != '\0') {
            while (cwd[ci] == '/') ci++;
            if (cwd[ci] == '\0') break;
            size_t start = ci;
            while (cwd[ci] != '\0' && cwd[ci] != '/') ci++;
            size_t len = ci - start;
            if (len == 1 && cwd[start] == '.') {
                continue;
            }
            if (len == 2 && cwd[start] == '.' && cwd[start + 1] == '.') {
                if (ncomps > 0) ncomps--;
                continue;
            }
            if (ncomps < 32) {
                comp_ptrs[ncomps] = cwd + start;
                comp_lens[ncomps] = len;
                ncomps++;
            }
        }
    }

    /* Parse input_path components */
    size_t pi = 0;
    while (input_path[pi] != '\0') {
        while (input_path[pi] == '/') pi++;
        if (input_path[pi] == '\0') break;
        size_t start = pi;
        while (input_path[pi] != '\0' && input_path[pi] != '/') pi++;
        size_t len = pi - start;
        if (len == 1 && input_path[start] == '.') {
            continue;
        }
        if (len == 2 && input_path[start] == '.' && input_path[start + 1] == '.') {
            if (ncomps > 0) {
                ncomps--;
            }
            continue;
        }
        if (ncomps < 32) {
            comp_ptrs[ncomps] = input_path + start;
            comp_lens[ncomps] = len;
            ncomps++;
        }
    }

    /* Reconstruct canonical path */
    size_t out_idx = 0;
    if (ncomps == 0) {
        out_buf[0] = '/';
        out_buf[1] = '\0';
        return 0;
    }

    for (int i = 0; i < ncomps; i++) {
        if (out_idx + 1 + comp_lens[i] >= out_capacity) {
            return -1;
        }
        out_buf[out_idx++] = '/';
        for (size_t k = 0; k < comp_lens[i]; k++) {
            out_buf[out_idx++] = comp_ptrs[i][k];
        }
    }
    out_buf[out_idx] = '\0';
    return 0;
}

int vfs_chdir(const char *path)
{
    task_t *task = scheduler_current_task();
    if (task == 0 || path == 0) {
        return -1;
    }

    char target_path[64];
    if (vfs_build_path(task->cwd, path, target_path, sizeof(target_path)) != 0) {
        return -1;
    }

    vfs_node_t *node = vfs_find(target_path);
    if (node == 0 || node->type != VFS_NODE_DIRECTORY) {
        return -1;
    }

    if (!vfs_check_permission(node, S_IXUSR, task->uid, task->gid)) {
        return -1;
    }

    size_t len = string_length(target_path);
    if (len >= sizeof(task->cwd)) {
        return -1;
    }

    for (size_t i = 0; i <= len; i++) {
        task->cwd[i] = target_path[i];
    }

    return 0;
}

int vfs_getcwd(char *buffer, size_t size)
{
    task_t *task = scheduler_current_task();
    if (task == 0 || buffer == 0 || size == 0) {
        return -1;
    }

    const char *cwd = (task->cwd[0] != '\0') ? task->cwd : "/";
    size_t len = string_length(cwd);
    if (size <= len) {
        return -1;
    }

    for (size_t i = 0; i <= len; i++) {
        buffer[i] = cwd[i];
    }

    return 0;
}

int vfs_truncate(vfs_node_t *node, size_t length)
{
    if (node == 0 || node->type != VFS_NODE_FILE) {
        return -1;
    }

    if (node->truncate != 0) {
        return node->truncate(node, length);
    }

    mutex_lock(&vfs_mutex);
    node->size = length;
    mutex_unlock(&vfs_mutex);
    return 0;
}

int vfs_truncate_path(const char *path, size_t length)
{
    if (path == 0) return -1;
    vfs_node_t *node = vfs_find(path);
    if (node == 0) return -1;
    return vfs_truncate(node, length);
}

int vfs_flock(vfs_node_t *node, int op, uint32_t pid)
{
    if (node == 0) {
        return -1;
    }

    mutex_lock(&vfs_mutex);

    if (op & LOCK_UN) {
        if (node->lock_type == LOCK_EX) {
            if (node->lock_owner_pid == pid || node->lock_owner_pid == 0) {
                node->lock_type = 0;
                node->lock_count = 0;
                node->lock_owner_pid = 0;
            }
        } else if (node->lock_type == LOCK_SH) {
            if (node->lock_count > 0) {
                node->lock_count--;
            }
            if (node->lock_count == 0) {
                node->lock_type = 0;
            }
        }
        mutex_unlock(&vfs_mutex);
        return 0;
    }

    if (op & LOCK_SH) {
        if (node->lock_type == LOCK_EX && node->lock_owner_pid != pid) {
            mutex_unlock(&vfs_mutex);
            return -1;
        }
        node->lock_type = LOCK_SH;
        node->lock_count++;
        mutex_unlock(&vfs_mutex);
        return 0;
    }

    if (op & LOCK_EX) {
        if ((node->lock_type == LOCK_EX && node->lock_owner_pid != pid) ||
            (node->lock_type == LOCK_SH && (node->lock_count > 1 || (node->lock_count == 1 && node->lock_owner_pid != pid)))) {
            mutex_unlock(&vfs_mutex);
            return -1;
        }
        node->lock_type = LOCK_EX;
        node->lock_count = 1;
        node->lock_owner_pid = pid;
        mutex_unlock(&vfs_mutex);
        return 0;
    }

    mutex_unlock(&vfs_mutex);
    return -1;
}

void vfs_node_unlock_by_pid(vfs_node_t *node, uint32_t pid)
{
    if (node == 0) {
        return;
    }

    mutex_lock(&vfs_mutex);
    if (node->lock_type == LOCK_EX && node->lock_owner_pid == pid) {
        node->lock_type = 0;
        node->lock_count = 0;
        node->lock_owner_pid = 0;
    } else if (node->lock_type == LOCK_SH) {
        if (node->lock_count > 0) {
            node->lock_count--;
        }
        if (node->lock_count == 0) {
            node->lock_type = 0;
        }
    }
    mutex_unlock(&vfs_mutex);
}

int vfs_sync(void)
{
    return bcache_sync();
}


