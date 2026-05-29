#include "vfs.h"

#include "heap.h"
#include "mutex.h"

static vfs_node_t *root_node;
static mutex_t vfs_mutex;

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
            return node;
        }
    }

    return 0;
}

void vfs_init(void)
{
    mutex_init(&vfs_mutex);
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

size_t vfs_read(vfs_node_t *node, size_t offset, size_t size, uint8_t *buffer)
{
    if (node == 0 || node->read == 0 || buffer == 0) {
        return 0;
    }

    mutex_lock(&vfs_mutex);
    size_t bytes = node->read(node, offset, size, buffer);
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
