#include "bcache.h"

#include "ata.h"
#include "mutex.h"
#include "serial.h"

struct bcache_entry {
    uint32_t lba;
    uint8_t data[BCACHE_SECTOR_SIZE] __attribute__((aligned(16)));
    uint8_t valid;
    uint8_t dirty;
    uint64_t last_access;
};

static struct bcache_entry cache_pool[BCACHE_BLOCK_COUNT];
static mutex_t bcache_mutex;
static uint64_t access_counter = 0;
static bcache_stats_t bcache_stats_data;

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

void bcache_init(void)
{
    mutex_init(&bcache_mutex);
    memory_zero(cache_pool, sizeof(cache_pool));
    memory_zero(&bcache_stats_data, sizeof(bcache_stats_data));
    access_counter = 0;
    klog("BCACHE: Buffer Cache inicializado (64 blocos, 32 KB).\n");
}

int bcache_read_sector(uint32_t lba, uint8_t *buffer)
{
    if (buffer == 0) {
        return 0;
    }

    mutex_lock(&bcache_mutex);
    bcache_stats_data.reads++;

    /* 1. Procura hit no cache */
    for (size_t i = 0; i < BCACHE_BLOCK_COUNT; i++) {
        if (cache_pool[i].valid && cache_pool[i].lba == lba) {
            bcache_stats_data.hits++;
            cache_pool[i].last_access = ++access_counter;
            memory_copy(buffer, cache_pool[i].data, BCACHE_SECTOR_SIZE);
            mutex_unlock(&bcache_mutex);
            return 1;
        }
    }

    /* 2. Cache Miss - Seleciona vítima via LRU */
    bcache_stats_data.misses++;

    int victim_idx = -1;
    uint64_t oldest_access = (uint64_t)-1;

    for (size_t i = 0; i < BCACHE_BLOCK_COUNT; i++) {
        if (!cache_pool[i].valid) {
            victim_idx = (int)i;
            break;
        }
        if (cache_pool[i].last_access < oldest_access) {
            oldest_access = cache_pool[i].last_access;
            victim_idx = (int)i;
        }
    }

    if (victim_idx < 0) {
        victim_idx = 0;
    }

    struct bcache_entry *victim = &cache_pool[victim_idx];

    /* Writeback se a vítima estava suja */
    if (victim->valid && victim->dirty) {
        if (!ata_write_sectors(victim->lba, 1, victim->data)) {
            mutex_unlock(&bcache_mutex);
            return 0;
        }
        victim->dirty = 0;
    }

    /* Lê do disco físico para a entrada do cache */
    if (!ata_read_sectors(lba, 1, victim->data)) {
        mutex_unlock(&bcache_mutex);
        return 0;
    }

    victim->lba = lba;
    victim->valid = 1;
    victim->dirty = 0;
    victim->last_access = ++access_counter;

    memory_copy(buffer, victim->data, BCACHE_SECTOR_SIZE);
    mutex_unlock(&bcache_mutex);
    return 1;
}

int bcache_write_sector(uint32_t lba, const uint8_t *buffer)
{
    if (buffer == 0) {
        return 0;
    }

    mutex_lock(&bcache_mutex);
    bcache_stats_data.writes++;

    struct bcache_entry *target = 0;

    for (size_t i = 0; i < BCACHE_BLOCK_COUNT; i++) {
        if (cache_pool[i].valid && cache_pool[i].lba == lba) {
            bcache_stats_data.hits++;
            target = &cache_pool[i];
            break;
        }
    }

    if (target == 0) {
        bcache_stats_data.misses++;
        int victim_idx = -1;
        uint64_t oldest_access = (uint64_t)-1;

        for (size_t i = 0; i < BCACHE_BLOCK_COUNT; i++) {
            if (!cache_pool[i].valid) {
                victim_idx = (int)i;
                break;
            }
            if (cache_pool[i].last_access < oldest_access) {
                oldest_access = cache_pool[i].last_access;
                victim_idx = (int)i;
            }
        }

        if (victim_idx < 0) {
            victim_idx = 0;
        }

        target = &cache_pool[victim_idx];
        if (target->valid && target->dirty) {
            ata_write_sectors(target->lba, 1, target->data);
            target->dirty = 0;
        }
        target->lba = lba;
        target->valid = 1;
    }

    memory_copy(target->data, buffer, BCACHE_SECTOR_SIZE);
    target->last_access = ++access_counter;
    target->dirty = 0;

    /* Write-through para consistência física imediata */
    int res = ata_write_sectors(lba, 1, buffer);
    if (!res) {
        target->dirty = 1; /* Marca dirty para tentativa posterior se falhou */
    }

    mutex_unlock(&bcache_mutex);
    return res;
}

int bcache_flush(void)
{
    mutex_lock(&bcache_mutex);
    bcache_stats_data.flushes++;

    for (size_t i = 0; i < BCACHE_BLOCK_COUNT; i++) {
        if (cache_pool[i].valid && cache_pool[i].dirty) {
            if (ata_write_sectors(cache_pool[i].lba, 1, cache_pool[i].data)) {
                cache_pool[i].dirty = 0;
            }
        }
    }

    mutex_unlock(&bcache_mutex);
    return 1;
}

void bcache_invalidate(uint32_t lba)
{
    mutex_lock(&bcache_mutex);
    for (size_t i = 0; i < BCACHE_BLOCK_COUNT; i++) {
        if (cache_pool[i].valid && cache_pool[i].lba == lba) {
            cache_pool[i].valid = 0;
            cache_pool[i].dirty = 0;
        }
    }
    mutex_unlock(&bcache_mutex);
}

int bcache_read_sectors(uint32_t lba, uint32_t count, uint8_t *buffer)
{
    if (buffer == 0 || count == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!bcache_read_sector(lba + i, buffer + (i * BCACHE_SECTOR_SIZE))) {
            return 0;
        }
    }

    return 1;
}

int bcache_write_sectors(uint32_t lba, uint32_t count, const uint8_t *buffer)
{
    if (buffer == 0 || count == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!bcache_write_sector(lba + i, buffer + (i * BCACHE_SECTOR_SIZE))) {
            return 0;
        }
    }

    return 1;
}

int bcache_sync(void)
{
    return bcache_flush();
}

void bcache_get_stats(bcache_stats_t *stats)
{
    if (stats == 0) return;
    mutex_lock(&bcache_mutex);
    memory_copy(stats, &bcache_stats_data, sizeof(*stats));
    mutex_unlock(&bcache_mutex);
}

