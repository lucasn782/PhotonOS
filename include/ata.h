#ifndef PHOTONOS_ATA_H
#define PHOTONOS_ATA_H

#include <stdint.h>

int ata_init(void);
int ata_read_sectors(uint32_t lba, uint8_t sector_count, uint8_t *buffer);
int ata_write_sectors(uint32_t lba, uint8_t sector_count,
    const uint8_t *buffer);
void ata_vfs_init(void);
int ata_vfs_create(const char *path);

#endif
