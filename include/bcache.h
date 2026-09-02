#ifndef PHOTONOS_BCACHE_H
#define PHOTONOS_BCACHE_H

#include <stddef.h>
#include <stdint.h>

#define BCACHE_SECTOR_SIZE 512U
#define BCACHE_BLOCK_COUNT 64U

typedef struct bcache_stats {
    uint64_t hits;
    uint64_t misses;
    uint64_t reads;
    uint64_t writes;
    uint64_t flushes;
} bcache_stats_t;

void bcache_init(void);
int bcache_read_sector(uint32_t lba, uint8_t *buffer);
int bcache_write_sector(uint32_t lba, const uint8_t *buffer);
int bcache_read_sectors(uint32_t lba, uint32_t count, uint8_t *buffer);
int bcache_write_sectors(uint32_t lba, uint32_t count, const uint8_t *buffer);
int bcache_flush(void);
int bcache_sync(void);
void bcache_invalidate(uint32_t lba);
void bcache_get_stats(bcache_stats_t *stats);

#endif
