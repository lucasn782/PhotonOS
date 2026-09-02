#include "fs/ext2.h"

#include "ata.h"
#include "bcache.h"
#include "heap.h"
#include "mutex.h"
#include "serial.h"
#include "vfs.h"

static struct ext2_superblock sb;
static struct ext2_group_desc *bg_desc = NULL;
static uint32_t num_groups = 0;
static uint32_t block_size = 1024;
static uint32_t partition_lba = 0;
static int ext2_mounted = 0;
static mutex_t ext2_mutex;

static void memory_copy(void *dest, const void *src, size_t size)
{
    uint8_t *out = dest;
    const uint8_t *in = src;
    for (size_t i = 0; i < size; i++) {
        out[i] = in[i];
    }
}

static void memory_zero(void *ptr, size_t size)
{
    uint8_t *bytes = ptr;
    for (size_t i = 0; i < size; i++) {
        bytes[i] = 0;
    }
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

static const char *path_basename(const char *path)
{
    const char *name = path;
    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            name = path + i + 1;
        }
    }
    return name;
}

static int ext2_read_block(uint32_t block, uint8_t *buf)
{
    return bcache_read_sectors(partition_lba + block * (block_size / 512), block_size / 512, buf);
}

static int ext2_write_block(uint32_t block, const uint8_t *buf)
{
    return bcache_write_sectors(partition_lba + block * (block_size / 512), block_size / 512, buf);
}

static uint32_t ext2_get_phys_block(struct ext2_inode *inode, uint32_t file_block_num)
{
    if (file_block_num < 12) {
        return inode->i_block[file_block_num];
    }
    
    uint32_t indirect_index = file_block_num - 12;
    uint32_t entries_per_block = block_size / 4;
    if (indirect_index < entries_per_block) {
        uint32_t sib = inode->i_block[12];
        if (sib == 0) return 0;
        
        uint32_t *indirect_buf = kmalloc(block_size);
        if (!indirect_buf) return 0;
        
        if (!ext2_read_block(sib, (uint8_t *)indirect_buf)) {
            kfree(indirect_buf);
            return 0;
        }
        
        uint32_t phys = indirect_buf[indirect_index];
        kfree(indirect_buf);
        return phys;
    }
    
    return 0;
}

static int ext2_read_file_block(struct ext2_inode *inode, uint32_t file_block_num, uint8_t *buf)
{
    uint32_t phys = ext2_get_phys_block(inode, file_block_num);
    if (phys == 0) {
        memory_zero(buf, block_size);
        return 1;
    }
    return ext2_read_block(phys, buf);
}

int ext2_read_inode(uint32_t inode_num, struct ext2_inode *out_inode)
{
    if (!ext2_mounted || inode_num == 0) return 0;
    
    uint32_t group = (inode_num - 1) / sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) % sb.s_inodes_per_group;
    uint32_t inode_size = (sb.s_rev_level == 0) ? 128 : sb.s_inode_size;
    
    if (group >= num_groups) return 0;
    
    uint32_t bg_inode_table = bg_desc[group].bg_inode_table;
    uint32_t block = bg_inode_table + (index * inode_size) / block_size;
    uint32_t offset_in_block = (index * inode_size) % block_size;
    
    uint8_t *block_buf = kmalloc(block_size);
    if (!block_buf) return 0;
    
    if (!bcache_read_sectors(partition_lba + block * (block_size / 512), block_size / 512, block_buf)) {
        kfree(block_buf);
        return 0;
    }
    
    memory_copy(out_inode, block_buf + offset_in_block, sizeof(struct ext2_inode));
    kfree(block_buf);
    return 1;
}

int ext2_write_inode(uint32_t inode_num, struct ext2_inode *inode)
{
    if (!ext2_mounted || inode_num == 0) return 0;
    
    uint32_t group = (inode_num - 1) / sb.s_inodes_per_group;
    uint32_t index = (inode_num - 1) % sb.s_inodes_per_group;
    uint32_t inode_size = (sb.s_rev_level == 0) ? 128 : sb.s_inode_size;
    
    if (group >= num_groups) return 0;
    
    uint32_t bg_inode_table = bg_desc[group].bg_inode_table;
    uint32_t block = bg_inode_table + (index * inode_size) / block_size;
    uint32_t offset_in_block = (index * inode_size) % block_size;
    
    uint8_t *block_buf = kmalloc(block_size);
    if (!block_buf) return 0;
    
    if (!bcache_read_sectors(partition_lba + block * (block_size / 512), block_size / 512, block_buf)) {
        kfree(block_buf);
        return 0;
    }
    
    memory_copy(block_buf + offset_in_block, inode, sizeof(struct ext2_inode));
    
    if (!bcache_write_sectors(partition_lba + block * (block_size / 512), block_size / 512, block_buf)) {
        kfree(block_buf);
        return 0;
    }
    
    kfree(block_buf);
    return 1;
}

int ext2_vfs_open(vfs_node_t *node)
{
    (void)node;
    return 0;
}

void ext2_vfs_close(vfs_node_t *node)
{
    (void)node;
}

int ext2_vfs_read(vfs_node_t *node, uint64_t offset, uint32_t size, uint8_t *buffer)
{
    if (!ext2_mounted || node == NULL || node->data == NULL || buffer == NULL) return 0;
    
    struct ext2_node_data *data = node->data;
    uint32_t inode_num = data->inode_num;
    if (inode_num == 0) return 0;
    
    struct ext2_inode inode;
    if (!ext2_read_inode(inode_num, &inode)) return 0;
    
    if (offset >= inode.i_size) return 0;
    if (offset + size > inode.i_size) {
        size = (uint32_t)(inode.i_size - offset);
    }
    
    uint32_t start_block = (uint32_t)(offset / block_size);
    uint32_t end_block = (uint32_t)((offset + size - 1) / block_size);
    uint32_t bytes_read = 0;
    
    uint8_t *block_buf = kmalloc(block_size);
    if (!block_buf) return 0;
    
    for (uint32_t b = start_block; b <= end_block; b++) {
        if (!ext2_read_file_block(&inode, b, block_buf)) {
            break;
        }
        
        uint32_t block_offset = 0;
        uint32_t chunk_size = block_size;
        
        if (b == start_block) {
            block_offset = (uint32_t)(offset % block_size);
            chunk_size = block_size - block_offset;
        }
        if (bytes_read + chunk_size > size) {
            chunk_size = size - bytes_read;
        }
        
        memory_copy(buffer + bytes_read, block_buf + block_offset, chunk_size);
        bytes_read += chunk_size;
    }
    
    kfree(block_buf);
    return bytes_read;
}

static int ext2_write_bg_descriptors(void)
{
    uint32_t bg_desc_block = sb.s_first_data_block + 1;
    uint32_t size_desc_table = num_groups * sizeof(struct ext2_group_desc);
    uint32_t sectors_to_write = (size_desc_table + 511) / 512;
    return bcache_write_sectors(partition_lba + bg_desc_block * (block_size / 512), sectors_to_write, (const uint8_t *)bg_desc);
}

static int ext2_write_superblock(void)
{
    uint8_t *sb_buf = kmalloc(1024);
    if (!sb_buf) return 0;
    memory_zero(sb_buf, 1024);
    memory_copy(sb_buf, &sb, sizeof(struct ext2_superblock));
    int res = bcache_write_sectors(partition_lba + 2, 2, sb_buf);
    kfree(sb_buf);
    return res;
}

static int find_free_bit(const uint8_t *bitmap, uint32_t max_bits)
{
    for (uint32_t i = 0; i < max_bits; i++) {
        uint32_t byte_idx = i / 8;
        uint32_t bit_idx = i % 8;
        if ((bitmap[byte_idx] & (1 << bit_idx)) == 0) {
            return (int)i;
        }
    }
    return -1;
}

uint32_t ext2_alloc_block(void)
{
    mutex_lock(&ext2_mutex);
    uint8_t *bitmap = kmalloc(block_size);
    if (!bitmap) {
        mutex_unlock(&ext2_mutex);
        return 0;
    }
    
    for (uint32_t g = 0; g < num_groups; g++) {
        if (bg_desc[g].bg_free_blocks_count > 0) {
            uint32_t bitmap_block = bg_desc[g].bg_block_bitmap;
            if (!ext2_read_block(bitmap_block, bitmap)) {
                continue;
            }
            
            int free_bit = find_free_bit(bitmap, sb.s_blocks_per_group);
            if (free_bit != -1) {
                bitmap[free_bit / 8] |= (1 << (free_bit % 8));
                if (!ext2_write_block(bitmap_block, bitmap)) {
                    continue;
                }
                
                bg_desc[g].bg_free_blocks_count--;
                (void)ext2_write_bg_descriptors();
                
                sb.s_free_blocks_count--;
                (void)ext2_write_superblock();
                
                uint32_t block_num = g * sb.s_blocks_per_group + free_bit + sb.s_first_data_block;
                kfree(bitmap);
                mutex_unlock(&ext2_mutex);
                return block_num;
            }
        }
    }
    
    kfree(bitmap);
    mutex_unlock(&ext2_mutex);
    return 0;
}

static int ext2_set_phys_block(struct ext2_inode *inode, uint32_t file_block_num, uint32_t phys_block)
{
    if (file_block_num < 12) {
        inode->i_block[file_block_num] = phys_block;
        return 1;
    }
    
    uint32_t indirect_index = file_block_num - 12;
    uint32_t entries_per_block = block_size / 4;
    if (indirect_index < entries_per_block) {
        uint32_t sib = inode->i_block[12];
        if (sib == 0) {
            sib = ext2_alloc_block();
            if (sib == 0) return 0;
            
            uint8_t *zero_buf = kmalloc(block_size);
            if (!zero_buf) return 0;
            memory_zero(zero_buf, block_size);
            if (!ext2_write_block(sib, zero_buf)) {
                kfree(zero_buf);
                return 0;
            }
            kfree(zero_buf);
            
            inode->i_block[12] = sib;
            inode->i_blocks += block_size / 512;
        }
        
        uint32_t *indirect_buf = kmalloc(block_size);
        if (!indirect_buf) return 0;
        
        if (!ext2_read_block(sib, (uint8_t *)indirect_buf)) {
            kfree(indirect_buf);
            return 0;
        }
        
        indirect_buf[indirect_index] = phys_block;
        
        if (!ext2_write_block(sib, (const uint8_t *)indirect_buf)) {
            kfree(indirect_buf);
            return 0;
        }
        
        kfree(indirect_buf);
        return 1;
    }
    
    return 0;
}

size_t ext2_vfs_write(vfs_node_t *node, size_t offset, size_t size, const uint8_t *buffer)
{
    if (!node || !node->data || !buffer) return 0;
    struct ext2_node_data *data = node->data;
    uint32_t inode_num = data->inode_num;
    struct ext2_inode inode;
    if (!ext2_read_inode(inode_num, &inode)) {
        return 0;
    }
    
    uint32_t current_blocks = (inode.i_size + block_size - 1) / block_size;
    uint32_t required_blocks = (offset + size + block_size - 1) / block_size;
    
    if (required_blocks > current_blocks) {
        for (uint32_t b = current_blocks; b < required_blocks; b++) {
            uint32_t phys = ext2_alloc_block();
            if (phys == 0) {
                break;
            }
            
            uint8_t *zero_buf = kmalloc(block_size);
            if (zero_buf) {
                memory_zero(zero_buf, block_size);
                (void)ext2_write_block(phys, zero_buf);
                kfree(zero_buf);
            }
            
            if (!ext2_set_phys_block(&inode, b, phys)) {
                break;
            }
            inode.i_blocks += block_size / 512;
        }
    }
    
    uint32_t bytes_written = 0;
    uint8_t *block_buf = kmalloc(block_size);
    if (!block_buf) return 0;
    
    while (bytes_written < size) {
        uint64_t cur_offset = offset + bytes_written;
        uint32_t file_block = cur_offset / block_size;
        uint32_t block_offset = cur_offset % block_size;
        uint32_t chunk_size = block_size - block_offset;
        if (chunk_size > size - bytes_written) {
            chunk_size = size - bytes_written;
        }
        
        uint32_t phys_block = ext2_get_phys_block(&inode, file_block);
        if (phys_block == 0) {
            break;
        }
        
        if (block_offset > 0 || chunk_size < block_size) {
            if (!ext2_read_block(phys_block, block_buf)) {
                break;
            }
        }
        
        memory_copy(block_buf + block_offset, buffer + bytes_written, chunk_size);
        
        if (!ext2_write_block(phys_block, block_buf)) {
            break;
        }
        
        bytes_written += chunk_size;
    }
    
    kfree(block_buf);
    
    if (offset + bytes_written > inode.i_size) {
        inode.i_size = offset + bytes_written;
    }
    
    if (!ext2_write_inode(inode_num, &inode)) {
        return 0;
    }
    
    node->size = inode.i_size;
    return bytes_written;
}

int ext2_vfs_readdir(vfs_node_t *node, uint32_t index, vfs_dir_entry_t *entry)
{
    if (!node || !node->data || !entry) return -1;
    struct ext2_node_data *data = node->data;
    struct ext2_inode inode;
    if (!ext2_read_inode(data->inode_num, &inode)) {
        return -1;
    }
    
    uint32_t dir_block_count = (inode.i_size + block_size - 1) / block_size;
    uint8_t *block_buf = kmalloc(block_size);
    if (!block_buf) return -1;
    
    uint32_t current_index = 0;
    int found = 0;
    
    for (uint32_t b = 0; b < dir_block_count && !found; b++) {
        if (!ext2_read_file_block(&inode, b, block_buf)) {
            break;
        }
        uint32_t offset = 0;
        while (offset < block_size) {
            struct ext2_dir_entry_2 *dir_entry = (struct ext2_dir_entry_2 *)(block_buf + offset);
            if (dir_entry->rec_len < 8 || offset + dir_entry->rec_len > block_size) {
                break;
            }
            if (dir_entry->inode != 0) {
                char name[256];
                uint32_t name_len = dir_entry->name_len < 255 ? dir_entry->name_len : 255;
                memory_copy(name, dir_entry->name, name_len);
                name[name_len] = '\0';
                
                int is_dot = (name_len == 1 && name[0] == '.');
                int is_dotdot = (name_len == 2 && name[0] == '.' && name[1] == '.');
                
                if (!is_dot && !is_dotdot) {
                    if (current_index == index) {
                        memory_zero(entry, sizeof(*entry));
                        copy_name(entry->name, name);
                        entry->size = 0;
                        entry->type = (dir_entry->file_type == 2) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
                        
                        struct ext2_inode entry_inode;
                        if (ext2_read_inode(dir_entry->inode, &entry_inode)) {
                            entry->size = entry_inode.i_size;
                        }
                        
                        found = 1;
                        break;
                    }
                    current_index++;
                }
            }
            offset += dir_entry->rec_len;
        }
    }
    
    kfree(block_buf);
    return found ? 1 : 0;
}

static void ext2_mount_dir(vfs_node_t *parent_node, uint32_t inode_num)
{
    struct ext2_inode inode;
    if (!ext2_read_inode(inode_num, &inode)) {
        return;
    }
    
    uint32_t dir_block_count = (inode.i_size + block_size - 1) / block_size;
    uint8_t *block_buf = kmalloc(block_size);
    if (!block_buf) return;
    
    for (uint32_t b = 0; b < dir_block_count; b++) {
        if (!ext2_read_file_block(&inode, b, block_buf)) {
            break;
        }
        uint32_t offset = 0;
        while (offset < block_size) {
            struct ext2_dir_entry_2 *entry = (struct ext2_dir_entry_2 *)(block_buf + offset);
            if (entry->rec_len < 8 || offset + entry->rec_len > block_size) {
                break;
            }
            if (entry->inode != 0) {
                char name[256];
                uint32_t len = entry->name_len < 255 ? entry->name_len : 255;
                memory_copy(name, entry->name, len);
                name[len] = '\0';
                
                int is_dot = (len == 1 && name[0] == '.');
                int is_dotdot = (len == 2 && name[0] == '.' && name[1] == '.');
                
                if (!is_dot && !is_dotdot) {
                    struct ext2_inode child_inode;
                    if (ext2_read_inode(entry->inode, &child_inode)) {
                        vfs_node_type_t type = (entry->file_type == 2) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
                        vfs_node_t *child_node = vfs_create_node(parent_node, name, type);
                        if (child_node != NULL) {
                            child_node->size = child_inode.i_size;
                            struct ext2_node_data *nd = kmalloc(sizeof(struct ext2_node_data));
                            if (nd != NULL) {
                                nd->inode_num = entry->inode;
                                child_node->data = nd;
                            }
                            child_node->read = ext2_vfs_read;
                            child_node->write = ext2_vfs_write;
                            child_node->open = ext2_vfs_open;
                            child_node->readdir = ext2_vfs_readdir;
                            
                            if (type == VFS_NODE_DIRECTORY) {
                                ext2_mount_dir(child_node, entry->inode);
                            }
                        }
                    }
                }
            }
            offset += entry->rec_len;
        }
    }
    
    kfree(block_buf);
}

int ext2_mount_at(vfs_node_t *mount_point, uint32_t partition_lba_val)
{
    if (mount_point == NULL) {
        return 0;
    }

    partition_lba = partition_lba_val;
    mutex_init(&ext2_mutex);
    
    uint8_t *sb_buf = kmalloc(1024);
    if (!sb_buf) return 0;
    
    if (!bcache_read_sectors(partition_lba + 2, 2, sb_buf)) {
        kfree(sb_buf);
        klog("EXT2: Falha ao ler o superbloco.\n");
        return 0;
    }
    
    memory_copy(&sb, sb_buf, sizeof(struct ext2_superblock));
    kfree(sb_buf);
    
    if (sb.s_magic != EXT2_SUPER_MAGIC) {
        klog("EXT2: Assinatura magica invalida.\n");
        return 0;
    }
    
    block_size = 1024 << sb.s_log_block_size;
    num_groups = (sb.s_blocks_count + sb.s_blocks_per_group - 1) / sb.s_blocks_per_group;
    
    uint32_t size_desc_table = num_groups * sizeof(struct ext2_group_desc);
    uint32_t sectors_to_read = (size_desc_table + 511) / 512;
    
    bg_desc = kmalloc(sectors_to_read * 512);
    if (!bg_desc) {
        klog("EXT2: Falha de alocacao para tabela de descritores.\n");
        return 0;
    }
    
    uint32_t bg_desc_block = sb.s_first_data_block + 1;
    if (!bcache_read_sectors(partition_lba + bg_desc_block * (block_size / 512), sectors_to_read, (uint8_t *)bg_desc)) {
        kfree(bg_desc);
        bg_desc = NULL;
        klog("EXT2: Falha ao ler descritores de grupo.\n");
        return 0;
    }
    
    ext2_mounted = 1;
    klog("EXT2: Sistema de ficheiros montado com sucesso.\n");
    
    mount_point->size = 0;
    struct ext2_inode root_inode;
    if (ext2_read_inode(2, &root_inode)) {
        mount_point->size = root_inode.i_size;
    }
    struct ext2_node_data *root_data = kmalloc(sizeof(struct ext2_node_data));
    if (root_data != 0) {
        root_data->inode_num = 2;
        mount_point->data = root_data;
    }
    mount_point->read = ext2_vfs_read;
    mount_point->write = ext2_vfs_write;
    mount_point->open = ext2_vfs_open;
    mount_point->readdir = ext2_vfs_readdir;
    
    ext2_mount_dir(mount_point, 2);
    return 1;
}

int ext2_mount(uint32_t partition_lba_val)
{
    return ext2_mount_at(vfs_root(), partition_lba_val);
}

uint32_t ext2_alloc_inode(void)
{
    mutex_lock(&ext2_mutex);
    uint8_t *bitmap = kmalloc(block_size);
    if (!bitmap) {
        mutex_unlock(&ext2_mutex);
        return 0;
    }
    
    for (uint32_t g = 0; g < num_groups; g++) {
        if (bg_desc[g].bg_free_inodes_count > 0) {
            uint32_t bitmap_block = bg_desc[g].bg_inode_bitmap;
            if (!ext2_read_block(bitmap_block, bitmap)) {
                continue;
            }
            
            int free_bit = find_free_bit(bitmap, sb.s_inodes_per_group);
            if (free_bit != -1) {
                bitmap[free_bit / 8] |= (1 << (free_bit % 8));
                if (!ext2_write_block(bitmap_block, bitmap)) {
                    continue;
                }
                
                bg_desc[g].bg_free_inodes_count--;
                (void)ext2_write_bg_descriptors();
                
                sb.s_free_inodes_count--;
                (void)ext2_write_superblock();
                
                uint32_t inode_num = g * sb.s_inodes_per_group + free_bit + 1;
                kfree(bitmap);
                mutex_unlock(&ext2_mutex);
                return inode_num;
            }
        }
    }
    
    kfree(bitmap);
    mutex_unlock(&ext2_mutex);
    return 0;
}

static int ext2_add_dir_entry(uint32_t dir_inode_num, const char *name, uint32_t inode_num, uint8_t file_type)
{
    struct ext2_inode dir_inode;
    if (!ext2_read_inode(dir_inode_num, &dir_inode)) {
        return 0;
    }
    
    uint32_t name_len = 0;
    while (name[name_len] != '\0') name_len++;
    if (name_len > 255) name_len = 255;
    
    uint32_t needed_len = 8 + ((name_len + 3) & ~3);
    uint32_t dir_block_count = (dir_inode.i_size + block_size - 1) / block_size;
    uint8_t *block_buf = kmalloc(block_size);
    if (!block_buf) return 0;
    
    for (uint32_t b = 0; b < dir_block_count; b++) {
        if (!ext2_read_file_block(&dir_inode, b, block_buf)) {
            break;
        }
        
        uint32_t offset = 0;
        while (offset < block_size) {
            struct ext2_dir_entry_2 *entry = (struct ext2_dir_entry_2 *)(block_buf + offset);
            if (entry->rec_len < 8 || offset + entry->rec_len > block_size) {
                break;
            }
            
            uint32_t min_rec_len = 8 + ((entry->name_len + 3) & ~3);
            if (entry->inode == 0) {
                if (entry->rec_len >= needed_len) {
                    entry->inode = inode_num;
                    entry->name_len = name_len;
                    entry->file_type = file_type;
                    memory_copy(entry->name, name, name_len);
                    if (!ext2_write_block(ext2_get_phys_block(&dir_inode, b), block_buf)) {
                        kfree(block_buf);
                        return 0;
                    }
                    kfree(block_buf);
                    return 1;
                }
            } else {
                if (entry->rec_len >= min_rec_len + needed_len) {
                    uint16_t old_rec_len = entry->rec_len;
                    entry->rec_len = min_rec_len;
                    
                    struct ext2_dir_entry_2 *new_entry = (struct ext2_dir_entry_2 *)(block_buf + offset + min_rec_len);
                    new_entry->inode = inode_num;
                    new_entry->rec_len = old_rec_len - min_rec_len;
                    new_entry->name_len = name_len;
                    new_entry->file_type = file_type;
                    memory_copy(new_entry->name, name, name_len);
                    
                    if (!ext2_write_block(ext2_get_phys_block(&dir_inode, b), block_buf)) {
                        kfree(block_buf);
                        return 0;
                    }
                    kfree(block_buf);
                    return 1;
                }
            }
            offset += entry->rec_len;
        }
    }
    
    if (dir_block_count >= 12) {
        kfree(block_buf);
        return 0;
    }
    
    uint32_t new_block = ext2_alloc_block();
    if (new_block == 0) {
        kfree(block_buf);
        return 0;
    }
    
    memory_zero(block_buf, block_size);
    struct ext2_dir_entry_2 *new_entry = (struct ext2_dir_entry_2 *)block_buf;
    new_entry->inode = inode_num;
    new_entry->rec_len = block_size;
    new_entry->name_len = name_len;
    new_entry->file_type = file_type;
    memory_copy(new_entry->name, name, name_len);
    
    if (!ext2_write_block(new_block, block_buf)) {
        kfree(block_buf);
        return 0;
    }
    
    dir_inode.i_block[dir_block_count] = new_block;
    dir_inode.i_size = (dir_block_count + 1) * block_size;
    dir_inode.i_blocks += block_size / 512;
    if (!ext2_write_inode(dir_inode_num, &dir_inode)) {
        kfree(block_buf);
        return 0;
    }
    
    kfree(block_buf);
    return 1;
}

int ext2_vfs_create(const char *path)
{
    const char *name = path_basename(path);
    if (name[0] == '\0') {
        return -1;
    }
    
    uint32_t new_inode_num = ext2_alloc_inode();
    if (new_inode_num == 0) {
        return -1;
    }
    
    struct ext2_inode new_inode;
    memory_zero(&new_inode, sizeof(new_inode));
    new_inode.i_mode = 0x81A4;
    new_inode.i_links_count = 1;
    new_inode.i_size = 0;
    new_inode.i_blocks = 0;
    if (!ext2_write_inode(new_inode_num, &new_inode)) {
        return -1;
    }
    
    if (!ext2_add_dir_entry(2, name, new_inode_num, EXT2_FT_REG_FILE)) {
        return -1;
    }
    
    vfs_node_t *node = vfs_find(path);
    if (node == 0) {
        node = vfs_create_node(vfs_root(), name, VFS_NODE_FILE);
    }
    if (node != 0) {
        struct ext2_node_data *node_data = kmalloc(sizeof(struct ext2_node_data));
        if (node_data != 0) {
            node_data->inode_num = new_inode_num;
            node->data = node_data;
        }
        node->size = 0;
        node->read = ext2_vfs_read;
        node->write = ext2_vfs_write;
        node->open = ext2_vfs_open;
        node->readdir = ext2_vfs_readdir;
    }
    
    return 0;
}
