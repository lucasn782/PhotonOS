#include "initrd.h"

#include <stddef.h>
#include <stdint.h>

#include "vfs.h"

struct initrd_file {
    const char *name;
    const uint8_t *data;
    size_t size;
};

extern const uint8_t _binary_shell_elf_start[];
extern const uint8_t _binary_shell_elf_end[];
extern const uint8_t _binary_hello_elf_start[];
extern const uint8_t _binary_hello_elf_end[];
extern const uint8_t _binary_upper_elf_start[];
extern const uint8_t _binary_upper_elf_end[];
extern const uint8_t _binary_rev_elf_start[];
extern const uint8_t _binary_rev_elf_end[];
extern const uint8_t _binary_hang_elf_start[];
extern const uint8_t _binary_hang_elf_end[];
extern const uint8_t _binary_spin_elf_start[];
extern const uint8_t _binary_spin_elf_end[];
extern const uint8_t _binary_ping_elf_start[];
extern const uint8_t _binary_ping_elf_end[];

static const uint8_t readme_txt[] =
    "PhotonOS initrd: kmalloc, VFS e sys_read ativos.\n";

static const struct initrd_file initrd_files[] = {
    { "readme.txt", readme_txt, sizeof(readme_txt) - 1 },
    { "bin/shell", _binary_shell_elf_start, 0 },
    { "bin/hello", _binary_hello_elf_start, 0 },
    { "bin/upper", _binary_upper_elf_start, 0 },
    { "bin/rev", _binary_rev_elf_start, 0 },
    { "bin/hang", _binary_hang_elf_start, 0 },
    { "bin/spin", _binary_spin_elf_start, 0 },
    { "bin/ping", _binary_ping_elf_start, 0 },
};

static int initrd_read(vfs_node_t *node, uint64_t offset, uint32_t size,
    uint8_t *buffer)
{
    const uint8_t *data = node->data;

    if (offset >= node->size) {
        return 0;
    }

    uint32_t available = (uint32_t)(node->size - offset);
    if (size > available) {
        size = available;
    }

    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = data[offset + i];
    }

    return (int)size;
}

void initrd_init(void)
{
    vfs_node_t *root = vfs_root();
    vfs_node_t *bin = vfs_create_node(root, "bin", VFS_NODE_DIRECTORY);

    for (size_t i = 0; i < sizeof(initrd_files) / sizeof(initrd_files[0]); i++) {
        const char *name = initrd_files[i].name;
        vfs_node_t *parent = root;

        if (name[0] == 'b' && name[1] == 'i' && name[2] == 'n' &&
            name[3] == '/') {
            parent = bin;
            name += 4;
        }

        vfs_node_t *node = vfs_create_node(parent, name, VFS_NODE_FILE);
        if (node == 0) {
            continue;
        }

        node->data = (void *)initrd_files[i].data;
        node->size = initrd_files[i].size;
        if (node->size == 0 && initrd_files[i].data == _binary_shell_elf_start) {
            node->size = (size_t)(_binary_shell_elf_end -
                _binary_shell_elf_start);
        } else if (node->size == 0 &&
            initrd_files[i].data == _binary_hello_elf_start) {
            node->size = (size_t)(_binary_hello_elf_end -
                _binary_hello_elf_start);
        } else if (node->size == 0 &&
            initrd_files[i].data == _binary_upper_elf_start) {
            node->size = (size_t)(_binary_upper_elf_end -
                _binary_upper_elf_start);
        } else if (node->size == 0 &&
            initrd_files[i].data == _binary_rev_elf_start) {
            node->size = (size_t)(_binary_rev_elf_end -
                _binary_rev_elf_start);
        } else if (node->size == 0 &&
            initrd_files[i].data == _binary_hang_elf_start) {
            node->size = (size_t)(_binary_hang_elf_end -
                _binary_hang_elf_start);
        } else if (node->size == 0 &&
            initrd_files[i].data == _binary_spin_elf_start) {
            node->size = (size_t)(_binary_spin_elf_end -
                _binary_spin_elf_start);
        } else if (node->size == 0 &&
            initrd_files[i].data == _binary_ping_elf_start) {
            node->size = (size_t)(_binary_ping_elf_end -
                _binary_ping_elf_start);
        }
        node->read = initrd_read;
    }
}
