#include "fat16.h"

#include "ata.h"
#include "bcache.h"
#include "heap.h"
#include "mutex.h"
#include "scheduler.h"
#include "serial.h"
#include "vfs.h"

#define FAT16_SECTOR_SIZE 512U
#define FAT16_FREE_CLUSTER 0x0000U
#define FAT16_EOC 0xFFF8U
#define FAT16_EOF 0xFFFFU
#define FAT16_BAD_CLUSTER 0xFFF7U
#define FAT16_DELETED_ENTRY 0xE5U
#define FAT16_BUFFER_ALIGNMENT 16U

struct fat16_volume {
    fat16_bpb_t bpb;
    uint32_t partition_lba;
    uint32_t fat_lba;
    uint32_t root_lba;
    uint32_t data_lba;
    uint32_t root_dir_sectors;
    uint32_t total_sectors;
    int mounted;
};

struct fat16_node_data {
    uint16_t first_cluster;
    uint32_t size;
    uint32_t dir_entry_lba;
    uint32_t dir_entry_index;
};

struct fat16_entry_location {
    uint32_t sector_lba;
    uint32_t index;
};

struct mbr_partition_entry {
    uint8_t bootable;
    uint8_t start_chs[3];
    uint8_t type;
    uint8_t end_chs[3];
    uint32_t start_lba;
    uint32_t sectors;
} __attribute__((packed));

static struct fat16_volume fat16;
static mutex_t fat16_mutex;
static uint8_t sector_buffer[FAT16_SECTOR_SIZE]
    __attribute__((aligned(FAT16_BUFFER_ALIGNMENT)));

static uint16_t read_u16(const void *ptr)
{
    const uint8_t *bytes = ptr;
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void write_u16(void *ptr, uint16_t value)
{
    uint8_t *bytes = ptr;

    bytes[0] = (uint8_t)(value & 0xFFU);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void write_u32(void *ptr, uint32_t value)
{
    uint8_t *bytes = ptr;

    bytes[0] = (uint8_t)(value & 0xFFU);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFU);
    bytes[2] = (uint8_t)((value >> 16) & 0xFFU);
    bytes[3] = (uint8_t)((value >> 24) & 0xFFU);
}

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

static int is_power_of_two(uint8_t value)
{
    return value != 0 && (value & (value - 1U)) == 0;
}

static int char_is_lower(char ch)
{
    return ch >= 'a' && ch <= 'z';
}

static char to_upper(char ch)
{
    if (char_is_lower(ch)) {
        return (char)(ch - 'a' + 'A');
    }

    return ch;
}

static char to_lower(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }

    return ch;
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

static int is_fat16_partition_type(uint8_t type)
{
    return type == 0x04 || type == 0x06 || type == 0x0E;
}

static int read_sector(uint32_t lba, uint8_t *buffer)
{
    return bcache_read_sector(lba, buffer);
}

static int write_sector(uint32_t lba, const uint8_t *buffer)
{
    return bcache_write_sector(lba, buffer);
}

static uint32_t root_dir_sectors(const fat16_bpb_t *bpb)
{
    uint32_t bytes = (uint32_t)bpb->root_entry_count * sizeof(fat16_entry_t);

    return (bytes + bpb->bytes_per_sector - 1U) / bpb->bytes_per_sector;
}

static uint32_t total_sectors(const fat16_bpb_t *bpb)
{
    if (bpb->total_sectors_16 != 0) {
        return bpb->total_sectors_16;
    }

    return bpb->total_sectors_32;
}

static int validate_bpb(const fat16_bpb_t *bpb, const uint8_t *sector)
{
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return 0;
    }
    if (bpb->bytes_per_sector != FAT16_SECTOR_SIZE) {
        return 0;
    }
    if (!is_power_of_two(bpb->sectors_per_cluster)) {
        return 0;
    }
    if (bpb->reserved_sectors == 0 || bpb->fat_count == 0 ||
        bpb->root_entry_count == 0 || bpb->sectors_per_fat == 0) {
        return 0;
    }
    if (total_sectors(bpb) == 0) {
        return 0;
    }

    return 1;
}

static int load_bpb_at(uint32_t partition_lba)
{
    if (!read_sector(partition_lba, sector_buffer)) {
        return 0;
    }

    fat16_bpb_t bpb;
    memory_copy(&bpb, sector_buffer, sizeof(bpb));
    if (!validate_bpb(&bpb, sector_buffer)) {
        return 0;
    }

    fat16.bpb = bpb;
    fat16.partition_lba = partition_lba;
    fat16.total_sectors = total_sectors(&bpb);
    fat16.root_dir_sectors = root_dir_sectors(&bpb);
    fat16.fat_lba = bpb.reserved_sectors;
    fat16.root_lba = fat16.fat_lba +
        ((uint32_t)bpb.fat_count * bpb.sectors_per_fat);
    fat16.data_lba = fat16.root_lba + fat16.root_dir_sectors;
    fat16.mounted = 1;

    klog("FAT16: Setor de Boot (BPB) validado com sucesso.\n");
    return 1;
}

static uint32_t cluster_to_lba(uint16_t cluster)
{
    return fat16.partition_lba + fat16.data_lba +
        ((uint32_t)(cluster - 2U) * fat16.bpb.sectors_per_cluster);
}

static int read_fat_entry(uint16_t cluster, uint16_t *next_cluster)
{
    uint32_t offset = (uint32_t)cluster * 2U;
    uint32_t sector = fat16.partition_lba + fat16.fat_lba +
        (offset / FAT16_SECTOR_SIZE);
    uint32_t sector_offset = offset % FAT16_SECTOR_SIZE;

    if (!read_sector(sector, sector_buffer)) {
        return 0;
    }

    *next_cluster = read_u16(sector_buffer + sector_offset);
    return 1;
}

static int write_fat_entry(uint16_t cluster, uint16_t value)
{
    uint32_t offset = (uint32_t)cluster * 2U;
    uint32_t sector_offset = offset % FAT16_SECTOR_SIZE;
    uint32_t fat_sector = offset / FAT16_SECTOR_SIZE;

    for (uint8_t fat_index = 0; fat_index < fat16.bpb.fat_count;
        fat_index++) {
        uint32_t sector = fat16.partition_lba + fat16.fat_lba +
            ((uint32_t)fat_index * fat16.bpb.sectors_per_fat) +
            fat_sector;

        if (!read_sector(sector, sector_buffer)) {
            return 0;
        }

        write_u16(sector_buffer + sector_offset, value);
        if (!write_sector(sector, sector_buffer)) {
            return 0;
        }
    }

    return 1;
}

static uint32_t cluster_size_bytes(void)
{
    return (uint32_t)fat16.bpb.sectors_per_cluster * FAT16_SECTOR_SIZE;
}

static uint32_t max_data_cluster(void)
{
    uint32_t data_sectors;
    uint32_t data_clusters;
    uint32_t fat_entries;
    uint32_t max_cluster;

    if (fat16.total_sectors <= fat16.data_lba) {
        return 1;
    }

    data_sectors = fat16.total_sectors - fat16.data_lba;
    data_clusters = data_sectors / fat16.bpb.sectors_per_cluster;
    fat_entries =
        ((uint32_t)fat16.bpb.sectors_per_fat * FAT16_SECTOR_SIZE) / 2U;

    if (data_clusters == 0 || fat_entries <= 2) {
        return 1;
    }

    max_cluster = data_clusters + 1U;
    if (max_cluster >= fat_entries) {
        max_cluster = fat_entries - 1U;
    }
    if (max_cluster >= FAT16_BAD_CLUSTER) {
        max_cluster = FAT16_BAD_CLUSTER - 1U;
    }

    return max_cluster;
}

static int cluster_is_data(uint16_t cluster)
{
    return cluster >= 2U && (uint32_t)cluster <= max_data_cluster();
}

static int clear_cluster(uint16_t cluster)
{
    uint32_t lba;

    if (!cluster_is_data(cluster)) {
        return 0;
    }

    lba = cluster_to_lba(cluster);
    memory_zero(sector_buffer, FAT16_SECTOR_SIZE);
    for (uint8_t sector = 0; sector < fat16.bpb.sectors_per_cluster;
        sector++) {
        if (!write_sector(lba + sector, sector_buffer)) {
            return 0;
        }
    }

    return 1;
}

static int allocate_cluster_unlocked(uint16_t *cluster_out)
{
    uint32_t max_cluster = max_data_cluster();

    if (!fat16.mounted || cluster_out == 0) {
        return 0;
    }

    for (uint32_t cluster = 2; cluster <= max_cluster; cluster++) {
        uint16_t value;

        if (!read_fat_entry((uint16_t)cluster, &value)) {
            return 0;
        }

        if (value != FAT16_FREE_CLUSTER) {
            continue;
        }

        if (!write_fat_entry((uint16_t)cluster, FAT16_EOF)) {
            return 0;
        }

        if (!clear_cluster((uint16_t)cluster)) {
            (void)write_fat_entry((uint16_t)cluster, FAT16_FREE_CLUSTER);
            return 0;
        }

        *cluster_out = (uint16_t)cluster;
        return 1;
    }

    return 0;
}

static int free_chain_from(uint16_t first_cluster)
{
    uint32_t guard = max_data_cluster();
    uint16_t cluster = first_cluster;

    while (cluster_is_data(cluster) && guard-- > 0) {
        uint16_t next;

        if (!read_fat_entry(cluster, &next)) {
            return 0;
        }
        if (!write_fat_entry(cluster, FAT16_FREE_CLUSTER)) {
            return 0;
        }

        if (next >= FAT16_EOC || next == FAT16_BAD_CLUSTER ||
            !cluster_is_data(next)) {
            break;
        }

        cluster = next;
    }

    return 1;
}

static int make_83_name(const char *filename, char out[11])
{
    const char *name = path_basename(filename);
    size_t input = 0;
    size_t output = 0;
    size_t base_length = 0;
    size_t ext_length = 0;

    for (size_t i = 0; i < 11; i++) {
        out[i] = ' ';
    }

    while (name[input] != '\0' && name[input] != '.') {
        if (base_length >= 8 || name[input] == ' ' || name[input] == '/' ||
            (uint8_t)name[input] < 0x20U) {
            return 0;
        }
        out[output++] = to_upper(name[input++]);
        base_length++;
    }

    if (name[input] == '.') {
        input++;
        output = 8;
        while (name[input] != '\0') {
            if (ext_length >= 3 || name[input] == '.' || name[input] == ' ' ||
                name[input] == '/' || (uint8_t)name[input] < 0x20U) {
                return 0;
            }
            out[output++] = to_upper(name[input++]);
            ext_length++;
        }
    }

    return base_length != 0;
}

static int entry_matches_name(const fat16_entry_t *entry, const char *filename)
{
    char fat_name[11];

    if (!make_83_name(filename, fat_name)) {
        return 0;
    }

    for (size_t i = 0; i < 8; i++) {
        if (entry->name[i] != fat_name[i]) {
            return 0;
        }
    }
    for (size_t i = 0; i < 3; i++) {
        if (entry->ext[i] != fat_name[8 + i]) {
            return 0;
        }
    }

    return 1;
}

static int entry_is_usable_file(const fat16_entry_t *entry)
{
    if ((uint8_t)entry->name[0] == 0x00 || (uint8_t)entry->name[0] == 0xE5) {
        return 0;
    }
    if ((entry->attributes & FAT16_ATTR_LONG_NAME) == FAT16_ATTR_LONG_NAME) {
        return 0;
    }
    if (entry->attributes & FAT16_ATTR_VOLUME_ID) {
        return 0;
    }
    if (entry->attributes & FAT16_ATTR_DIRECTORY) {
        return 0;
    }

    return 1;
}

static int lookup_root_entry(const char *filename, fat16_entry_t *out_entry,
    struct fat16_entry_location *out_location)
{
    uint32_t root_lba = fat16.partition_lba + fat16.root_lba;

    for (uint32_t sector = 0; sector < fat16.root_dir_sectors; sector++) {
        if (!read_sector(root_lba + sector, sector_buffer)) {
            return 0;
        }

        fat16_entry_t *entries = (fat16_entry_t *)sector_buffer;
        for (uint32_t i = 0; i < FAT16_SECTOR_SIZE / sizeof(fat16_entry_t);
            i++) {
            if ((uint8_t)entries[i].name[0] == 0x00) {
                return 0;
            }
            if (!entry_is_usable_file(&entries[i])) {
                continue;
            }
            if (entry_matches_name(&entries[i], filename)) {
                if (out_entry != 0) {
                    *out_entry = entries[i];
                }
                if (out_location != 0) {
                    out_location->sector_lba = root_lba + sector;
                    out_location->index = i;
                }
                return 1;
            }
        }
    }

    return 0;
}

static int find_free_root_slot(struct fat16_entry_location *location)
{
    uint32_t root_lba = fat16.partition_lba + fat16.root_lba;

    for (uint32_t sector = 0; sector < fat16.root_dir_sectors; sector++) {
        if (!read_sector(root_lba + sector, sector_buffer)) {
            return 0;
        }

        fat16_entry_t *entries = (fat16_entry_t *)sector_buffer;
        for (uint32_t i = 0; i < FAT16_SECTOR_SIZE / sizeof(fat16_entry_t);
            i++) {
            uint8_t first = (uint8_t)entries[i].name[0];

            if (first == 0x00U || first == FAT16_DELETED_ENTRY) {
                location->sector_lba = root_lba + sector;
                location->index = i;
                return 1;
            }
        }
    }

    return 0;
}

static int fill_entry(fat16_entry_t *entry, const char *filename,
    uint16_t starting_cluster, uint32_t filesize)
{
    char fat_name[11];

    if (!make_83_name(filename, fat_name) || fat_name[0] == ' ') {
        return 0;
    }

    memory_zero(entry, sizeof(*entry));
    for (size_t i = 0; i < 8; i++) {
        entry->name[i] = fat_name[i];
    }
    for (size_t i = 0; i < 3; i++) {
        entry->ext[i] = fat_name[8 + i];
    }
    entry->attributes = FAT16_ATTR_ARCHIVE;
    write_u16((uint8_t *)entry + offsetof(fat16_entry_t, first_cluster_high),
        0);
    write_u16((uint8_t *)entry + offsetof(fat16_entry_t, starting_cluster),
        starting_cluster);
    write_u32((uint8_t *)entry + offsetof(fat16_entry_t, size), filesize);

    return 1;
}

static int create_entry_unlocked(const char *filename,
    uint16_t starting_cluster, uint32_t filesize,
    struct fat16_entry_location *location_out, fat16_entry_t *entry_out)
{
    struct fat16_entry_location location;

    if (lookup_root_entry(filename, 0, 0)) {
        return 0;
    }

    if (!find_free_root_slot(&location)) {
        return 0;
    }

    fat16_entry_t *entries = (fat16_entry_t *)sector_buffer;
    fat16_entry_t *entry = &entries[location.index];

    if (!fill_entry(entry, filename, starting_cluster, filesize)) {
        return 0;
    }

    if (!write_sector(location.sector_lba, sector_buffer)) {
        return 0;
    }

    if (location_out != 0) {
        *location_out = location;
    }
    if (entry_out != 0) {
        *entry_out = *entry;
    }

    return 1;
}

static int update_entry_unlocked(const struct fat16_node_data *data)
{
    if (data == 0 || data->dir_entry_lba == 0 ||
        data->dir_entry_index >= FAT16_SECTOR_SIZE / sizeof(fat16_entry_t)) {
        return 0;
    }

    if (!read_sector(data->dir_entry_lba, sector_buffer)) {
        return 0;
    }

    fat16_entry_t *entries = (fat16_entry_t *)sector_buffer;
    fat16_entry_t *entry = &entries[data->dir_entry_index];
    write_u16((uint8_t *)entry + offsetof(fat16_entry_t, starting_cluster),
        data->first_cluster);
    write_u32((uint8_t *)entry + offsetof(fat16_entry_t, size), data->size);
    entry->attributes |= FAT16_ATTR_ARCHIVE;

    return write_sector(data->dir_entry_lba, sector_buffer);
}

static int update_entry_at_unlocked(const struct fat16_entry_location *location,
    uint16_t first_cluster, uint32_t size, fat16_entry_t *entry_out)
{
    if (location == 0 ||
        location->index >= FAT16_SECTOR_SIZE / sizeof(fat16_entry_t)) {
        return 0;
    }

    if (!read_sector(location->sector_lba, sector_buffer)) {
        return 0;
    }

    fat16_entry_t *entries = (fat16_entry_t *)sector_buffer;
    fat16_entry_t *entry = &entries[location->index];
    write_u16((uint8_t *)entry + offsetof(fat16_entry_t, starting_cluster),
        first_cluster);
    write_u32((uint8_t *)entry + offsetof(fat16_entry_t, size), size);
    entry->attributes |= FAT16_ATTR_ARCHIVE;

    if (!write_sector(location->sector_lba, sector_buffer)) {
        return 0;
    }

    if (entry_out != 0) {
        *entry_out = *entry;
    }

    return 1;
}

static size_t read_chain(uint16_t first_cluster, uint32_t file_size,
    size_t offset, size_t size, uint8_t *buffer)
{
    if (first_cluster < 2 || offset >= file_size || buffer == 0) {
        return 0;
    }

    if (size > file_size - offset) {
        size = file_size - offset;
    }

    uint32_t cluster_size =
        (uint32_t)fat16.bpb.sectors_per_cluster * FAT16_SECTOR_SIZE;
    uint32_t cluster_skip = (uint32_t)(offset / cluster_size);
    uint32_t offset_in_cluster = (uint32_t)(offset % cluster_size);
    uint16_t cluster = first_cluster;

    for (uint32_t i = 0; i < cluster_skip; i++) {
        if (!read_fat_entry(cluster, &cluster) || cluster >= FAT16_EOC ||
            cluster == FAT16_BAD_CLUSTER) {
            return 0;
        }
    }

    size_t copied = 0;
    while (copied < size && cluster < FAT16_EOC &&
        cluster != FAT16_BAD_CLUSTER) {
        uint32_t cluster_lba = cluster_to_lba(cluster);

        for (uint8_t sector = 0; sector < fat16.bpb.sectors_per_cluster &&
            copied < size; sector++) {
            uint32_t cluster_byte_offset =
                (uint32_t)sector * FAT16_SECTOR_SIZE;
            if (cluster_byte_offset + FAT16_SECTOR_SIZE <=
                offset_in_cluster) {
                continue;
            }

            if (!read_sector(cluster_lba + sector, sector_buffer)) {
                return copied;
            }

            uint32_t sector_offset = 0;
            if (offset_in_cluster > cluster_byte_offset) {
                sector_offset = offset_in_cluster - cluster_byte_offset;
            }

            size_t chunk = FAT16_SECTOR_SIZE - sector_offset;
            if (chunk > size - copied) {
                chunk = size - copied;
            }

            memory_copy(buffer + copied, sector_buffer + sector_offset, chunk);
            copied += chunk;
        }

        offset_in_cluster = 0;
        if (copied < size &&
            !read_fat_entry(cluster, &cluster)) {
            break;
        }
    }

    return copied;
}

static uint32_t clusters_required_for_size(uint32_t size)
{
    uint32_t cluster_size = cluster_size_bytes();

    if (size == 0) {
        return 1;
    }

    return (size + cluster_size - 1U) / cluster_size;
}

static int ensure_chain_capacity(struct fat16_node_data *data,
    uint32_t end_offset)
{
    uint32_t required_clusters = clusters_required_for_size(end_offset);
    uint16_t cluster = data->first_cluster;

    if (!cluster_is_data(cluster)) {
        if (!allocate_cluster_unlocked(&cluster)) {
            return 0;
        }
        data->first_cluster = cluster;
    }

    for (uint32_t count = 1; count < required_clusters; count++) {
        uint16_t next;

        if (!read_fat_entry(cluster, &next)) {
            return 0;
        }

        if (next >= FAT16_EOC) {
            if (!allocate_cluster_unlocked(&next)) {
                return 0;
            }
            if (!write_fat_entry(cluster, next)) {
                (void)write_fat_entry(next, FAT16_FREE_CLUSTER);
                return 0;
            }
        } else if (!cluster_is_data(next) || next == FAT16_BAD_CLUSTER ||
            next == FAT16_FREE_CLUSTER) {
            return 0;
        }

        cluster = next;
    }

    return 1;
}

static int cluster_for_offset(uint16_t first_cluster, uint32_t offset,
    uint16_t *cluster_out, uint32_t *offset_in_cluster_out)
{
    uint32_t cluster_size = cluster_size_bytes();
    uint32_t cluster_skip = offset / cluster_size;
    uint16_t cluster = first_cluster;

    if (!cluster_is_data(cluster)) {
        return 0;
    }

    for (uint32_t i = 0; i < cluster_skip; i++) {
        uint16_t next;

        if (!read_fat_entry(cluster, &next) || !cluster_is_data(next) ||
            next >= FAT16_EOC || next == FAT16_BAD_CLUSTER) {
            return 0;
        }
        cluster = next;
    }

    *cluster_out = cluster;
    *offset_in_cluster_out = offset % cluster_size;
    return 1;
}

static size_t write_chain(struct fat16_node_data *data, size_t offset,
    size_t size, const uint8_t *buffer)
{
    uint64_t end_offset_64 = (uint64_t)offset + (uint64_t)size;
    uint16_t cluster;
    uint32_t offset_in_cluster;
    size_t written = 0;

    if (data == 0 || buffer == 0 || size == 0 ||
        offset > 0xFFFFFFFFULL || end_offset_64 > 0xFFFFFFFFULL) {
        return 0;
    }

    if (!ensure_chain_capacity(data, (uint32_t)end_offset_64)) {
        return 0;
    }

    if (!cluster_for_offset(data->first_cluster, (uint32_t)offset,
        &cluster, &offset_in_cluster)) {
        return 0;
    }

    while (written < size && cluster_is_data(cluster)) {
        uint32_t cluster_lba = cluster_to_lba(cluster);

        for (uint8_t sector = 0; sector < fat16.bpb.sectors_per_cluster &&
            written < size; sector++) {
            uint32_t cluster_byte_offset =
                (uint32_t)sector * FAT16_SECTOR_SIZE;
            uint32_t sector_offset = 0;
            size_t chunk;

            if (cluster_byte_offset + FAT16_SECTOR_SIZE <=
                offset_in_cluster) {
                continue;
            }

            if (offset_in_cluster > cluster_byte_offset) {
                sector_offset = offset_in_cluster - cluster_byte_offset;
            }

            chunk = FAT16_SECTOR_SIZE - sector_offset;
            if (chunk > size - written) {
                chunk = size - written;
            }

            if (sector_offset != 0 || chunk != FAT16_SECTOR_SIZE) {
                if (!read_sector(cluster_lba + sector, sector_buffer)) {
                    goto done;
                }
            }

            memory_copy(sector_buffer + sector_offset, buffer + written,
                chunk);
            if (!write_sector(cluster_lba + sector, sector_buffer)) {
                goto done;
            }

            written += chunk;
        }

        offset_in_cluster = 0;
        if (written < size) {
            uint16_t next;

            if (!read_fat_entry(cluster, &next) || !cluster_is_data(next) ||
                next >= FAT16_EOC || next == FAT16_BAD_CLUSTER) {
                break;
            }
            cluster = next;
        }
    }

done:
    if (written > 0) {
        uint32_t new_end = (uint32_t)((uint64_t)offset + written);

        if (new_end > data->size) {
            data->size = new_end;
        }
        if (!update_entry_unlocked(data)) {
            return 0;
        }
    }

    return written;
}

static int fat16_open(vfs_node_t *node)
{
    (void)node;
    return 0;
}

static int fat16_read(vfs_node_t *node, uint64_t offset, uint32_t size,
    uint8_t *buffer)
{
    struct fat16_node_data *data = node->data;
    int bytes;

    if (data == 0) {
        return 0;
    }

    mutex_lock(&fat16_mutex);
    bytes = (int)read_chain(data->first_cluster, data->size, (size_t)offset,
        (size_t)size, buffer);
    mutex_unlock(&fat16_mutex);

    return bytes;
}

static size_t fat16_vfs_write(vfs_node_t *node, size_t offset, size_t size,
    const uint8_t *buffer)
{
    struct fat16_node_data *data = node->data;
    size_t bytes;

    if (data == 0) {
        return 0;
    }

    mutex_lock(&fat16_mutex);
    bytes = write_chain(data, offset, size, buffer);
    node->size = data->size;
    mutex_unlock(&fat16_mutex);

    return bytes;
}

static void entry_to_vfs_name(const fat16_entry_t *entry, char *name)
{
    size_t output = 0;

    for (size_t i = 0; i < 8 && entry->name[i] != ' '; i++) {
        name[output++] = to_lower(entry->name[i]);
    }

    if (entry->ext[0] != ' ') {
        name[output++] = '.';
        for (size_t i = 0; i < 3 && entry->ext[i] != ' '; i++) {
            name[output++] = to_lower(entry->ext[i]);
        }
    }

    name[output] = '\0';
}

static vfs_node_t *ensure_directory(vfs_node_t *parent, const char *name)
{
    vfs_node_t *node = vfs_find(name);

    if (parent != vfs_root()) {
        node = 0;
    }

    if (node != 0 && node->type == VFS_NODE_DIRECTORY) {
        return node;
    }

    return vfs_create_node(parent, name, VFS_NODE_DIRECTORY);
}

static struct fat16_node_data *create_node_data(const fat16_entry_t *entry,
    const struct fat16_entry_location *location)
{
    struct fat16_node_data *data = kmalloc(sizeof(*data));

    if (data == 0) {
        return 0;
    }

    data->first_cluster = entry->starting_cluster;
    data->size = entry->size;
    data->dir_entry_lba = location != 0 ? location->sector_lba : 0;
    data->dir_entry_index = location != 0 ? location->index : 0;
    return data;
}

static void mount_file(vfs_node_t *parent, const char *name,
    const fat16_entry_t *entry, const struct fat16_entry_location *location)
{
    vfs_node_t *node = vfs_create_node(parent, name, VFS_NODE_FILE);
    if (node == 0) {
        return;
    }

    node->data = create_node_data(entry, location);
    if (node->data == 0) {
        return;
    }
    node->size = entry->size;
    node->read = fat16_read;
    node->open = fat16_open;
    node->write = fat16_vfs_write;
    node->truncate = fat16_vfs_truncate;
}

static int get_subdir_entry(uint16_t first_cluster, uint32_t entry_index, fat16_entry_t *out_entry)
{
    uint32_t entries_per_sector = FAT16_SECTOR_SIZE / sizeof(fat16_entry_t);
    uint32_t entries_per_cluster = entries_per_sector * fat16.bpb.sectors_per_cluster;
    
    uint32_t cluster_skip = entry_index / entries_per_cluster;
    uint32_t index_in_cluster = entry_index % entries_per_cluster;
    uint32_t sector_in_cluster = index_in_cluster / entries_per_sector;
    uint32_t index_in_sector = index_in_cluster % entries_per_sector;
    
    uint16_t cluster = first_cluster;
    for (uint32_t i = 0; i < cluster_skip; i++) {
        uint16_t next;
        if (!read_fat_entry(cluster, &next)) {
            return -1;
        }
        if (next >= FAT16_EOC || next == FAT16_BAD_CLUSTER || next == FAT16_FREE_CLUSTER) {
            return 0;
        }
        cluster = next;
    }
    
    uint32_t sector_lba = cluster_to_lba(cluster) + sector_in_cluster;
    if (!read_sector(sector_lba, sector_buffer)) {
        return -1;
    }
    
    fat16_entry_t *entries = (fat16_entry_t *)sector_buffer;
    *out_entry = entries[index_in_sector];
    return 1;
}

static int fat16_dir_entry_unlocked(vfs_node_t *dir, uint32_t entry_index,
    fat16_entry_t *entry, struct fat16_entry_location *location)
{
    uint32_t entries_per_sector = FAT16_SECTOR_SIZE / sizeof(fat16_entry_t);
    uint32_t sector_lba;
    uint32_t index_in_sector;

    if (dir == 0 || entry == 0) return -1;
    if (dir == vfs_root()) {
        if (entry_index >= fat16.bpb.root_entry_count) return 0;
        sector_lba = fat16.partition_lba + fat16.root_lba +
            (entry_index / entries_per_sector);
        index_in_sector = entry_index % entries_per_sector;
    } else {
        struct fat16_node_data *data = dir->data;
        uint32_t entries_per_cluster;
        uint32_t cluster_skip;
        uint32_t index_in_cluster;
        uint16_t cluster;

        if (dir->type != VFS_NODE_DIRECTORY || data == 0 ||
            !cluster_is_data(data->first_cluster)) return -1;
        entries_per_cluster = entries_per_sector * fat16.bpb.sectors_per_cluster;
        if (entries_per_cluster == 0 || entry_index >=
            max_data_cluster() * entries_per_cluster) return 0;
        cluster_skip = entry_index / entries_per_cluster;
        index_in_cluster = entry_index % entries_per_cluster;
        cluster = data->first_cluster;
        for (uint32_t i = 0; i < cluster_skip; i++) {
            uint16_t next;
            if (!read_fat_entry(cluster, &next)) return -1;
            if (!cluster_is_data(next) || next >= FAT16_EOC ||
                next == FAT16_BAD_CLUSTER) return 0;
            cluster = next;
        }
        sector_lba = cluster_to_lba(cluster) +
            (index_in_cluster / entries_per_sector);
        index_in_sector = index_in_cluster % entries_per_sector;
    }
    if (!read_sector(sector_lba, sector_buffer)) return -1;
    *entry = ((fat16_entry_t *)sector_buffer)[index_in_sector];
    if (location != 0) {
        location->sector_lba = sector_lba;
        location->index = index_in_sector;
    }
    return 1;
}

static int fat16_entry_is_visible(const fat16_entry_t *entry)
{
    return (uint8_t)entry->name[0] != 0x00U &&
        (uint8_t)entry->name[0] != FAT16_DELETED_ENTRY &&
        (entry->attributes & FAT16_ATTR_VOLUME_ID) == 0 &&
        (entry->attributes & FAT16_ATTR_LONG_NAME) != FAT16_ATTR_LONG_NAME;
}

static int fat16_lookup_dir_entry_unlocked(vfs_node_t *dir, const char *name,
    fat16_entry_t *entry_out, struct fat16_entry_location *location_out)
{
    for (uint32_t index = 0;; index++) {
        fat16_entry_t entry;
        struct fat16_entry_location location;
        int result = fat16_dir_entry_unlocked(dir, index, &entry, &location);
        if (result <= 0 || (uint8_t)entry.name[0] == 0x00U) return 0;
        if (fat16_entry_is_visible(&entry) && entry_matches_name(&entry, name)) {
            if (entry_out != 0) *entry_out = entry;
            if (location_out != 0) *location_out = location;
            return 1;
        }
    }
}

static int fat16_find_free_dir_slot_unlocked(vfs_node_t *dir,
    struct fat16_entry_location *location_out)
{
    for (uint32_t index = 0;; index++) {
        fat16_entry_t entry;
        struct fat16_entry_location location;
        int result = fat16_dir_entry_unlocked(dir, index, &entry, &location);
        if (result <= 0) return 0;
        if ((uint8_t)entry.name[0] == 0x00U ||
            (uint8_t)entry.name[0] == FAT16_DELETED_ENTRY) {
            *location_out = location;
            return 1;
        }
    }
}

static int fat16_store_dir_entry_unlocked(
    const struct fat16_entry_location *location, const fat16_entry_t *entry)
{
    if (location == 0 || entry == 0 ||
        location->index >= FAT16_SECTOR_SIZE / sizeof(fat16_entry_t) ||
        !read_sector(location->sector_lba, sector_buffer)) return 0;
    ((fat16_entry_t *)sector_buffer)[location->index] = *entry;
    return write_sector(location->sector_lba, sector_buffer);
}

static int fat16_directory_empty_unlocked(vfs_node_t *dir)
{
    for (uint32_t index = 0;; index++) {
        fat16_entry_t entry;
        int result = fat16_dir_entry_unlocked(dir, index, &entry, 0);
        if (result <= 0 || (uint8_t)entry.name[0] == 0x00U) return result >= 0;
        if (fat16_entry_is_visible(&entry)) {
            char name[VFS_NAME_MAX];
            entry_to_vfs_name(&entry, name);
            if (!(name[0] == '.' && (name[1] == '\0' ||
                (name[1] == '.' && name[2] == '\0')))) return 0;
        }
    }
}

static int fat16_split_path(const char *path, vfs_node_t **parent_out,
    char name[VFS_NAME_MAX])
{
    size_t length = 0;
    size_t last_slash = 0;
    size_t name_start;
    char parent_path[VFS_NAME_MAX];

    if (path == 0 || parent_out == 0 || name == 0) return 0;
    while (path[length] != '\0') {
        if (path[length] == '/') last_slash = length;
        length++;
    }
    name_start = (last_slash == 0 && path[0] != '/') ? 0 : last_slash + 1;
    if (length == 0 || name_start >= length || length - name_start >= VFS_NAME_MAX) {
        return 0;
    }
    if (last_slash == 0) {
        parent_path[0] = '/';
        parent_path[1] = '\0';
    } else {
        if (last_slash >= sizeof(parent_path)) return 0;
        for (size_t i = 0; i < last_slash; i++) parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
    }
    for (size_t i = 0; i < length - name_start; i++) name[i] = path[name_start + i];
    name[length - name_start] = '\0';
    *parent_out = vfs_find(parent_path);
    return *parent_out != 0 && (*parent_out == vfs_root() ||
        ((*parent_out)->type == VFS_NODE_DIRECTORY && (*parent_out)->data != 0));
}

static int fat16_readdir(vfs_node_t *node, uint32_t index, vfs_dir_entry_t *entry);

static int fat16_vfs_create_path(const char *path)
{
    char name[VFS_NAME_MAX];
    vfs_node_t *parent;
    fat16_entry_t entry;
    struct fat16_entry_location location;
    int result = -1;

    if (!fat16.mounted || !fat16_split_path(path, &parent, name)) return -1;
    mutex_lock(&fat16_mutex);
    if (fat16_lookup_dir_entry_unlocked(parent, name, &entry, &location)) {
        result = (entry.attributes & FAT16_ATTR_DIRECTORY) ? -1 : 0;
    } else if (fat16_find_free_dir_slot_unlocked(parent, &location) &&
        fill_entry(&entry, name, 0, 0) &&
        fat16_store_dir_entry_unlocked(&location, &entry)) {
        result = 0;
    }
    mutex_unlock(&fat16_mutex);
    if (result != 0) return -1;

    vfs_node_t *node = vfs_find(path);
    if (node == 0) {
        mount_file(parent, name, &entry, &location);
        node = vfs_find(path);
    }
    if (node != 0) {
        task_t *task = scheduler_current_task();
        uint32_t umask = task ? task->umask : 0022;
        node->mode = (0666 & ~umask) & 0777;
        node->uid = task ? task->uid : 0;
        node->gid = task ? task->gid : 0;
    }
    return node != 0 && node->type == VFS_NODE_FILE ? 0 : -1;
}

static void fat16_init_dot_entry(fat16_entry_t *entry, int parent,
    uint16_t cluster)
{
    memory_zero(entry, sizeof(*entry));
    for (size_t i = 0; i < 8; i++) entry->name[i] = ' ';
    for (size_t i = 0; i < 3; i++) entry->ext[i] = ' ';
    entry->name[0] = '.';
    if (parent) entry->name[1] = '.';
    entry->attributes = FAT16_ATTR_DIRECTORY;
    entry->starting_cluster = cluster;
}

static int fat16_vfs_mkdir_path(const char *path, uint32_t mode)
{
    char name[VFS_NAME_MAX];
    vfs_node_t *parent;
    fat16_entry_t entry;
    fat16_entry_t dot;
    fat16_entry_t dotdot;
    struct fat16_entry_location location;
    uint16_t cluster = 0;
    uint16_t parent_cluster = 0;
    int result = -1;

    if (!fat16.mounted || !fat16_split_path(path, &parent, name)) return -1;
    mutex_lock(&fat16_mutex);
    if (fat16_lookup_dir_entry_unlocked(parent, name, 0, 0) ||
        !allocate_cluster_unlocked(&cluster)) goto done;
    if (parent != vfs_root()) parent_cluster =
        ((struct fat16_node_data *)parent->data)->first_cluster;
    fat16_init_dot_entry(&dot, 0, cluster);
    fat16_init_dot_entry(&dotdot, 1, parent_cluster);
    memory_zero(sector_buffer, FAT16_SECTOR_SIZE);
    ((fat16_entry_t *)sector_buffer)[0] = dot;
    ((fat16_entry_t *)sector_buffer)[1] = dotdot;
    if (!write_sector(cluster_to_lba(cluster), sector_buffer) ||
        !fat16_find_free_dir_slot_unlocked(parent, &location) ||
        !fill_entry(&entry, name, cluster, 0)) goto rollback;
    entry.attributes = FAT16_ATTR_DIRECTORY;
    if (!fat16_store_dir_entry_unlocked(&location, &entry)) goto rollback;
    result = 0;
    goto done;
rollback:
    (void)free_chain_from(cluster);
done:
    mutex_unlock(&fat16_mutex);
    if (result != 0) return -1;

    vfs_node_t *node = vfs_find(path);
    if (node == 0) node = vfs_create_node(parent, name, VFS_NODE_DIRECTORY);
    if (node == 0) return -1;
    task_t *task = scheduler_current_task();
    uint32_t umask = task ? task->umask : 0022;
    node->mode = mode ? ((mode & ~umask) & 0777) : ((0777 & ~umask) & 0777);
    node->uid = task ? task->uid : 0;
    node->gid = task ? task->gid : 0;
    node->data = create_node_data(&entry, &location);
    if (node->data == 0) return -1;
    node->size = 0;
    node->readdir = fat16_readdir;
    return 0;
}

static int fat16_vfs_rmdir_path(const char *path)
{
    char name[VFS_NAME_MAX];
    vfs_node_t *parent;
    fat16_entry_t entry;
    struct fat16_entry_location location;
    struct fat16_node_data data;
    vfs_node_t temporary;
    int result = -1;

    if (!fat16.mounted || !fat16_split_path(path, &parent, name)) return -1;
    mutex_lock(&fat16_mutex);
    if (!fat16_lookup_dir_entry_unlocked(parent, name, &entry, &location) ||
        (entry.attributes & FAT16_ATTR_DIRECTORY) == 0) goto done;
    memory_zero(&temporary, sizeof(temporary));
    data.first_cluster = entry.starting_cluster;
    temporary.type = VFS_NODE_DIRECTORY;
    temporary.data = &data;
    if (!fat16_directory_empty_unlocked(&temporary)) goto done;
    entry.name[0] = (char)FAT16_DELETED_ENTRY;
    if (!fat16_store_dir_entry_unlocked(&location, &entry)) goto done;
    if (cluster_is_data(data.first_cluster) && !free_chain_from(data.first_cluster)) {
        goto done;
    }
    result = 0;
done:
    mutex_unlock(&fat16_mutex);
    return result;
}

static int fat16_vfs_unlink_path(const char *path)
{
    char name[VFS_NAME_MAX];
    vfs_node_t *parent;
    fat16_entry_t entry;
    struct fat16_entry_location location;
    int result = -1;

    if (!fat16.mounted || !fat16_split_path(path, &parent, name)) return -1;
    mutex_lock(&fat16_mutex);
    if (!fat16_lookup_dir_entry_unlocked(parent, name, &entry, &location) ||
        (entry.attributes & FAT16_ATTR_DIRECTORY) != 0) goto done;
    entry.name[0] = (char)FAT16_DELETED_ENTRY;
    if (!fat16_store_dir_entry_unlocked(&location, &entry)) goto done;
    if (cluster_is_data(entry.starting_cluster) && !free_chain_from(entry.starting_cluster)) {
        goto done;
    }
    result = 0;
done:
    mutex_unlock(&fat16_mutex);
    return result;
}

static int fat16_readdir(vfs_node_t *node, uint32_t index, vfs_dir_entry_t *entry)
{
    if (node == 0 || entry == 0) {
        return -1;
    }

    mutex_lock(&fat16_mutex);

    if (node == vfs_root()) {
        uint32_t root_lba = fat16.partition_lba + fat16.root_lba;
        uint32_t entries_per_sector = FAT16_SECTOR_SIZE / sizeof(fat16_entry_t);
        uint32_t i = 0;
        uint32_t usable_count = 0;

        while (i < fat16.bpb.root_entry_count) {
            uint32_t sector = root_lba + (i / entries_per_sector);
            uint32_t idx_in_sector = i % entries_per_sector;

            if (!read_sector(sector, sector_buffer)) {
                mutex_unlock(&fat16_mutex);
                return -1;
            }

            fat16_entry_t *entries = (fat16_entry_t *)sector_buffer;
            fat16_entry_t *ent = &entries[idx_in_sector];

            if ((uint8_t)ent->name[0] == 0x00) {
                mutex_unlock(&fat16_mutex);
                return 0;
            }

            if ((uint8_t)ent->name[0] != 0xE5 &&
                (ent->attributes & FAT16_ATTR_VOLUME_ID) == 0 &&
                (ent->attributes & FAT16_ATTR_LONG_NAME) != FAT16_ATTR_LONG_NAME) {

                if (usable_count == index) {
                    entry_to_vfs_name(ent, entry->name);
                    entry->size = ent->size;
                    entry->type = (ent->attributes & FAT16_ATTR_DIRECTORY) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;

                    mutex_unlock(&fat16_mutex);
                    return 1;
                }
                usable_count++;
            }
            i++;
        }

        mutex_unlock(&fat16_mutex);
        return 0;
    }

    struct fat16_node_data *data = node->data;
    if (data == 0) {
        mutex_unlock(&fat16_mutex);
        return -1;
    }

    uint16_t first_cluster = data->first_cluster;
    uint32_t i = 0;
    uint32_t usable_count = 0;

    for (;;) {
        fat16_entry_t ent;
        int res = get_subdir_entry(first_cluster, i, &ent);
        if (res == 0) {
            mutex_unlock(&fat16_mutex);
            return 0;
        }
        if (res < 0) {
            mutex_unlock(&fat16_mutex);
            return -1;
        }

        if ((uint8_t)ent.name[0] == 0x00) {
            mutex_unlock(&fat16_mutex);
            return 0;
        }

        if ((uint8_t)ent.name[0] != 0xE5 &&
            (ent.attributes & FAT16_ATTR_VOLUME_ID) == 0 &&
            (ent.attributes & FAT16_ATTR_LONG_NAME) != FAT16_ATTR_LONG_NAME) {

            if (usable_count == index) {
                entry_to_vfs_name(&ent, entry->name);
                entry->size = ent.size;
                entry->type = (ent.attributes & FAT16_ATTR_DIRECTORY) ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;

                mutex_unlock(&fat16_mutex);
                return 1;
            }
            usable_count++;
        }
        i++;
    }
}

static void mount_dir_entries_recursive(vfs_node_t *parent_node, uint16_t first_cluster)
{
    if (!cluster_is_data(first_cluster)) {
        return;
    }

    uint8_t *local_buffer = kmalloc(FAT16_SECTOR_SIZE);
    if (local_buffer == 0) {
        return;
    }

    uint16_t cluster = first_cluster;
    uint32_t entries_per_sector = FAT16_SECTOR_SIZE / sizeof(fat16_entry_t);

    while (cluster_is_data(cluster) && cluster < FAT16_EOC && cluster != FAT16_BAD_CLUSTER) {
        uint32_t cluster_lba = cluster_to_lba(cluster);

        for (uint8_t sector = 0; sector < fat16.bpb.sectors_per_cluster; sector++) {
            if (!read_sector(cluster_lba + sector, local_buffer)) {
                kfree(local_buffer);
                return;
            }

            fat16_entry_t *entries = (fat16_entry_t *)local_buffer;
            for (uint32_t i = 0; i < entries_per_sector; i++) {
                if ((uint8_t)entries[i].name[0] == 0x00) {
                    kfree(local_buffer);
                    return;
                }

                if ((uint8_t)entries[i].name[0] != 0xE5 &&
                    (entries[i].attributes & FAT16_ATTR_VOLUME_ID) == 0 &&
                    (entries[i].attributes & FAT16_ATTR_LONG_NAME) != FAT16_ATTR_LONG_NAME) {

                    char name[VFS_NAME_MAX];
                    entry_to_vfs_name(&entries[i], name);
                    if (name[0] != '\0') {
                        if (!(name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))) {
                            struct fat16_entry_location location;
                            location.sector_lba = cluster_lba + sector;
                            location.index = i;

                            if (entries[i].attributes & FAT16_ATTR_DIRECTORY) {
                                vfs_node_t *sub_node = vfs_create_node(parent_node, name, VFS_NODE_DIRECTORY);
                                if (sub_node != 0) {
                                    sub_node->data = create_node_data(&entries[i], &location);
                                    sub_node->size = entries[i].size;
                                    sub_node->readdir = fat16_readdir;
                                    mount_dir_entries_recursive(sub_node, entries[i].starting_cluster);
                                }
                            } else {
                                mount_file(parent_node, name, &entries[i], &location);
                            }
                        }
                    }
                }
            }
        }

        uint16_t next;
        if (!read_fat_entry(cluster, &next)) {
            break;
        }
        if (next >= FAT16_EOC || next == FAT16_BAD_CLUSTER || next == FAT16_FREE_CLUSTER) {
            break;
        }
        cluster = next;
    }

    kfree(local_buffer);
}

static void mount_root_entries(void)
{
    vfs_node_t *root = vfs_root();
    vfs_node_t *bin = ensure_directory(root, "bin");
    uint32_t root_lba = fat16.partition_lba + fat16.root_lba;

    root->readdir = fat16_readdir;

    for (uint32_t sector = 0; sector < fat16.root_dir_sectors; sector++) {
        if (!read_sector(root_lba + sector, sector_buffer)) {
            return;
        }

        fat16_entry_t *entries = (fat16_entry_t *)sector_buffer;
        for (uint32_t i = 0; i < FAT16_SECTOR_SIZE / sizeof(fat16_entry_t);
            i++) {
            if ((uint8_t)entries[i].name[0] == 0x00) {
                return;
            }
            if ((uint8_t)entries[i].name[0] == 0xE5 ||
                (entries[i].attributes & FAT16_ATTR_VOLUME_ID) ||
                (entries[i].attributes & FAT16_ATTR_LONG_NAME) == FAT16_ATTR_LONG_NAME) {
                continue;
            }

            char name[VFS_NAME_MAX];
            entry_to_vfs_name(&entries[i], name);
            if (name[0] == '\0') {
                continue;
            }

            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
                continue;
            }

            struct fat16_entry_location location;
            location.sector_lba = root_lba + sector;
            location.index = i;

            if (entries[i].attributes & FAT16_ATTR_DIRECTORY) {
                vfs_node_t *sub_node = vfs_create_node(root, name, VFS_NODE_DIRECTORY);
                if (sub_node != 0) {
                    sub_node->data = create_node_data(&entries[i], &location);
                    sub_node->size = entries[i].size;
                    sub_node->readdir = fat16_readdir;
                    mount_dir_entries_recursive(sub_node, entries[i].starting_cluster);
                }
            } else {
                mount_file(root, name, &entries[i], &location);
                if (bin != 0) {
                    mount_file(bin, name, &entries[i], &location);
                }
            }
        }
    }
}

static int mount_superfloppy_or_partition(uint32_t requested_lba)
{
    if (load_bpb_at(requested_lba)) {
        return 1;
    }

    if (requested_lba != 0 || !read_sector(0, sector_buffer)) {
        return 0;
    }

    if (sector_buffer[510] != 0x55 || sector_buffer[511] != 0xAA) {
        return 0;
    }

    struct mbr_partition_entry *parts =
        (struct mbr_partition_entry *)(sector_buffer + 446);
    for (uint32_t i = 0; i < 4; i++) {
        if (parts[i].bootable == 0x80) {
            if (is_fat16_partition_type(parts[i].type)) {
                if (load_bpb_at(parts[i].start_lba)) {
                    return 1;
                }
            }
        }
    }

    for (uint32_t i = 0; i < 4; i++) {
        if (is_fat16_partition_type(parts[i].type)) {
            if (load_bpb_at(parts[i].start_lba)) {
                return 1;
            }
        }
    }

    return 0;
}

int fat16_mount(uint32_t partition_lba)
{
    memory_zero(&fat16, sizeof(fat16));

    if (!mount_superfloppy_or_partition(partition_lba)) {
        klog("FAT16: nenhum volume valido encontrado.\n");
        return 0;
    }

    klog("FAT16: Particao ativa montada em /dev/ata0p1.\n");
    mount_root_entries();
    klog("FAT16: volume montado via VFS.\n");
    return 1;
}

int fat16_is_mounted(void)
{
    return fat16.mounted;
}

int fat16_read_file(const char *filename, void *buffer)
{
    if (!fat16.mounted || filename == 0 || buffer == 0) {
        return -1;
    }

    fat16_entry_t entry;
    if (!lookup_root_entry(filename, &entry, 0)) {
        return -1;
    }

    size_t bytes = read_chain(entry.starting_cluster, entry.size, 0,
        entry.size, buffer);
    return bytes == entry.size ? (int)bytes : -1;
}

static uint32_t file_clusters_required(size_t size)
{
    uint32_t cluster_size = cluster_size_bytes();

    if (size == 0 || cluster_size == 0) {
        return 0;
    }
    if (size > 0xFFFFFFFFULL) {
        return 0xFFFFFFFFU;
    }

    return ((uint32_t)size + cluster_size - 1U) / cluster_size;
}

static int resize_chain_unlocked(uint16_t *first_cluster,
    uint32_t required_clusters)
{
    if (first_cluster == 0) {
        return 0;
    }

    if (required_clusters == 0) {
        if (cluster_is_data(*first_cluster) &&
            !free_chain_from(*first_cluster)) {
            return 0;
        }
        *first_cluster = 0;
        return 1;
    }

    if (!cluster_is_data(*first_cluster)) {
        if (!allocate_cluster_unlocked(first_cluster)) {
            return 0;
        }
    }

    uint16_t cluster = *first_cluster;
    for (uint32_t count = 1; count < required_clusters; count++) {
        uint16_t next;

        if (!read_fat_entry(cluster, &next)) {
            return 0;
        }

        if (next >= FAT16_EOC) {
            if (!allocate_cluster_unlocked(&next)) {
                return 0;
            }
            if (!write_fat_entry(cluster, next)) {
                (void)free_chain_from(next);
                return 0;
            }
        } else if (!cluster_is_data(next) || next == FAT16_BAD_CLUSTER ||
            next == FAT16_FREE_CLUSTER) {
            return 0;
        }

        cluster = next;
    }

    uint16_t next;
    if (!read_fat_entry(cluster, &next)) {
        return 0;
    }

    if (cluster_is_data(next)) {
        if (!write_fat_entry(cluster, FAT16_EOF)) {
            return 0;
        }
        return free_chain_from(next);
    }

    if (next != FAT16_EOF && next != FAT16_EOC) {
        return write_fat_entry(cluster, FAT16_EOF);
    }

    return 1;
}

static int write_file_content_unlocked(uint16_t first_cluster,
    const uint8_t *buffer, size_t size)
{
    size_t written = 0;
    uint16_t cluster = first_cluster;
    uint32_t guard = max_data_cluster();

    if (size == 0) {
        return 1;
    }
    if (buffer == 0 || !cluster_is_data(cluster)) {
        return 0;
    }

    while (written < size && cluster_is_data(cluster) && guard-- > 0) {
        uint32_t cluster_lba = cluster_to_lba(cluster);

        for (uint8_t sector = 0; sector < fat16.bpb.sectors_per_cluster &&
            written < size; sector++) {
            size_t remaining = size - written;
            size_t chunk = remaining;

            if (chunk > FAT16_SECTOR_SIZE) {
                chunk = FAT16_SECTOR_SIZE;
            }

            if (chunk == FAT16_SECTOR_SIZE) {
                if (!ata_write_sectors(cluster_lba + sector, 1,
                    buffer + written)) {
                    return 0;
                }
            } else {
                memory_zero(sector_buffer, FAT16_SECTOR_SIZE);
                memory_copy(sector_buffer, buffer + written, chunk);
                if (!ata_write_sectors(cluster_lba + sector, 1,
                    sector_buffer)) {
                    return 0;
                }
            }

            written += chunk;
        }

        if (written < size) {
            uint16_t next;

            if (!read_fat_entry(cluster, &next) ||
                !cluster_is_data(next) || next >= FAT16_EOC ||
                next == FAT16_BAD_CLUSTER) {
                return 0;
            }
            cluster = next;
        }
    }

    return written == size;
}

int fat16_write_file(const char *filename, const void *buffer, size_t size)
{
    struct fat16_entry_location location = {0};
    fat16_entry_t entry;
    uint16_t first_cluster;
    uint32_t required_clusters;
    int exists;
    int result = -1;

    if (!fat16.mounted || filename == 0 ||
        (buffer == 0 && size != 0) || size > 0xFFFFFFFFULL) {
        return -1;
    }

    mutex_lock(&fat16_mutex);

    exists = lookup_root_entry(filename, &entry, &location);

    if (!exists) {
        first_cluster = 0;
        if (size != 0 && !allocate_cluster_unlocked(&first_cluster)) {
            goto done;
        }
        if (!create_entry_unlocked(filename, first_cluster, 0, &location,
            &entry)) {
            if (cluster_is_data(first_cluster)) {
                (void)free_chain_from(first_cluster);
            }
            goto done;
        }
    }

    first_cluster = entry.starting_cluster;
    required_clusters = file_clusters_required(size);
    if (required_clusters == 0xFFFFFFFFU ||
        !resize_chain_unlocked(&first_cluster, required_clusters)) {
        goto done;
    }

    if (!write_file_content_unlocked(first_cluster, buffer, size)) {
        goto done;
    }

    if (!update_entry_at_unlocked(&location, first_cluster, (uint32_t)size,
        &entry)) {
        goto done;
    }

    result = (int)size;

done:
    mutex_unlock(&fat16_mutex);
    return result;
}

int fat16_vfs_create(const char *path)
{
    return fat16_vfs_create_path(path);
}

int fat16_vfs_mkdir(const char *path, uint32_t mode)
{
    return fat16_vfs_mkdir_path(path, mode);
}

int fat16_vfs_rmdir(const char *path)
{
    return fat16_vfs_rmdir_path(path);
}

int fat16_vfs_unlink(const char *path)
{
    return fat16_vfs_unlink_path(path);
}

int fat16_vfs_truncate(vfs_node_t *node, size_t length)
{
    if (node == 0 || node->type != VFS_NODE_FILE) {
        return -1;
    }
    struct fat16_node_data *data = node->data;
    if (data == 0) {
        return -1;
    }

    if (!fat16.mounted || length > 0xFFFFFFFFULL) {
        return -1;
    }

    mutex_lock(&fat16_mutex);

    uint32_t req_clusters = file_clusters_required(length);
    if (req_clusters == 0xFFFFFFFFU) {
        mutex_unlock(&fat16_mutex);
        return -1;
    }

    if (!resize_chain_unlocked(&data->first_cluster, req_clusters)) {
        mutex_unlock(&fat16_mutex);
        return -1;
    }

    data->size = (uint32_t)length;
    node->size = length;

    if (!update_entry_unlocked(data)) {
        mutex_unlock(&fat16_mutex);
        return -1;
    }

    mutex_unlock(&fat16_mutex);
    return 0;
}
