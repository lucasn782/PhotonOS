#ifndef PHOTONOS_FAT16_H
#define PHOTONOS_FAT16_H

#include <stddef.h>
#include <stdint.h>

#define FAT16_ATTR_READ_ONLY 0x01U
#define FAT16_ATTR_HIDDEN 0x02U
#define FAT16_ATTR_SYSTEM 0x04U
#define FAT16_ATTR_VOLUME_ID 0x08U
#define FAT16_ATTR_DIRECTORY 0x10U
#define FAT16_ATTR_ARCHIVE 0x20U
#define FAT16_ATTR_LONG_NAME 0x0FU

typedef struct __attribute__((packed)) fat16_bpb {
    uint8_t boot_jmp[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
} fat16_bpb_t;

typedef struct __attribute__((packed)) fat16_entry {
    char name[8];
    char ext[3];
    uint8_t attributes;
    uint8_t nt_reserved;
    uint8_t creation_tenth;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t starting_cluster;
    uint32_t size;
} fat16_entry_t;

int fat16_mount(uint32_t partition_lba);
int fat16_read_file(const char *filename, void *buffer);
int fat16_write_file(const char *filename, const void *buffer, size_t size);
int fat16_vfs_create(const char *path);

#endif
