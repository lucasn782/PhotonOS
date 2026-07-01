#include "ata.h"

#include <stddef.h>
#include <stdint.h>

#include "fat16.h"
#include "mutex.h"
#include "serial.h"
#include "vfs.h"
#include "fs/ext2.h"

#define ATA_DATA 0x1F0
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LOW 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HIGH 0x1F5
#define ATA_DRIVE 0x1F6
#define ATA_STATUS 0x1F7
#define ATA_COMMAND 0x1F7
#define ATA_CONTROL 0x3F6

#define ATA_STATUS_ERR 0x01
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_BSY 0x80

#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_CACHE_FLUSH 0xE7

static int ata_present;
static mutex_t ata_mutex;

static uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void insw(uint16_t port, void *buffer, uint32_t count)
{
    __asm__ volatile ("rep insw"
        : "+D"(buffer), "+c"(count)
        : "d"(port)
        : "memory");
}

static void outsw(uint16_t port, const void *buffer, uint32_t count)
{
    __asm__ volatile ("rep outsw"
        : "+S"(buffer), "+c"(count)
        : "d"(port)
        : "memory");
}

static void ata_delay_400ns(void)
{
    (void)inb(ATA_CONTROL);
    (void)inb(ATA_CONTROL);
    (void)inb(ATA_CONTROL);
    (void)inb(ATA_CONTROL);
}

static int ata_wait_ready(void)
{
    for (uint32_t i = 0; i < 1000000U; i++) {
        uint8_t status = inb(ATA_STATUS);
        if ((status & ATA_STATUS_BSY) == 0) {
            return (status & ATA_STATUS_ERR) == 0;
        }
    }

    return 0;
}

static int ata_wait_drq(void)
{
    for (uint32_t i = 0; i < 1000000U; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_STATUS_ERR) {
            return 0;
        }
        if ((status & ATA_STATUS_BSY) == 0 && (status & ATA_STATUS_DRQ)) {
            return 1;
        }
    }

    return 0;
}

int ata_init(void)
{
    uint16_t identify[256];

    mutex_init(&ata_mutex);

    outb(ATA_CONTROL, 0);
    outb(ATA_DRIVE, 0xE0);
    ata_delay_400ns();

    if (inb(ATA_STATUS) == 0) {
        ata_present = 0;
        klog("ATA: nenhum disco no barramento primario.\n");
        return 0;
    }

    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns();

    if (inb(ATA_STATUS) == 0 || !ata_wait_drq()) {
        ata_present = 0;
        klog("ATA: IDENTIFY falhou.\n");
        return 0;
    }

    insw(ATA_DATA, identify, 256);
    ata_present = 1;
    klog("ATA: disco primario detectado em modo PIO.\n");
    return 1;
}

int ata_read_sectors(uint32_t lba, uint8_t sector_count, uint8_t *buffer)
{
    if (!ata_present || sector_count == 0 || buffer == 0 ||
        lba > 0x0FFFFFFFU) {
        return 0;
    }

    mutex_lock(&ata_mutex);

    if (!ata_wait_ready()) {
        mutex_unlock(&ata_mutex);
        return 0;
    }

    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SECTOR_COUNT, sector_count);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ_PIO);

    for (uint8_t sector = 0; sector < sector_count; sector++) {
        if (!ata_wait_drq()) {
            mutex_unlock(&ata_mutex);
            return 0;
        }
        insw(ATA_DATA, buffer + ((uint32_t)sector * 512U), 256);
        ata_delay_400ns();
    }

    mutex_unlock(&ata_mutex);
    return 1;
}

int ata_write_sectors(uint32_t lba, uint8_t sector_count,
    const uint8_t *buffer)
{
    if (!ata_present || sector_count == 0 || buffer == 0 ||
        lba > 0x0FFFFFFFU) {
        return 0;
    }

    mutex_lock(&ata_mutex);

    if (!ata_wait_ready()) {
        mutex_unlock(&ata_mutex);
        return 0;
    }

    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SECTOR_COUNT, sector_count);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);

    for (uint8_t sector = 0; sector < sector_count; sector++) {
        if (!ata_wait_drq()) {
            mutex_unlock(&ata_mutex);
            return 0;
        }
        outsw(ATA_DATA, buffer + ((uint32_t)sector * 512U), 256);
        ata_delay_400ns();
    }

    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    int res = ata_wait_ready();
    mutex_unlock(&ata_mutex);
    return res;
}

static int ata_block_read(vfs_node_t *node, uint64_t offset, uint32_t size,
    uint8_t *buffer)
{
    (void)node;

    if ((offset % 512U) != 0 || (size % 512U) != 0 || size == 0) {
        return 0;
    }
    if (!ata_read_sectors((uint32_t)(offset / 512U), (uint8_t)(size / 512U),
        buffer)) {
        return 0;
    }

    return (int)size;
}

static size_t ata_block_write(vfs_node_t *node, size_t offset, size_t size,
    const uint8_t *buffer)
{
    (void)node;

    if ((offset % 512U) != 0 || (size % 512U) != 0 || size == 0) {
        return 0;
    }
    if (!ata_write_sectors((uint32_t)(offset / 512U), (uint8_t)(size / 512U),
        buffer)) {
        return 0;
    }

    return size;
}

void ata_vfs_init(void)
{
    vfs_node_t *dev;
    vfs_node_t *hda;

    if (!ata_present) {
        return;
    }

    dev = vfs_find("/dev");
    if (dev == 0) {
        dev = vfs_create_node(vfs_root(), "dev", VFS_NODE_DIRECTORY);
    }

    hda = vfs_create_node(dev, "hda", VFS_NODE_DEVICE);
    if (hda != 0) {
        hda->read = ata_block_read;
        hda->write = ata_block_write;
    }

    if (!fat16_mount(0)) {
        (void)ext2_mount(0);
    }
}

int ata_vfs_create(const char *path)
{
    int ret = fat16_vfs_create(path);
    if (ret < 0) {
        return ext2_vfs_create(path);
    }
    return ret;
}
