#include "e1000.h"

#include "heap.h"
#include "memory.h"
#include "mutex.h"
#include "serial.h"
#include "vmm.h"

#define E1000_PACKET_BUFFER_SIZE 2048
#define E1000_TX_POLL_LIMIT 1000000U

#define E1000_MMIO_VADDR 0x00000000F0000000ULL
#define E1000_MMIO_SIZE 0x20000ULL
#define E1000_RX_RING_VADDR 0x00000000F0020000ULL
#define E1000_TX_RING_VADDR 0x00000000F0021000ULL

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed, aligned(16)));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed, aligned(16)));

struct e1000_state {
    volatile uint8_t *mmio;
    uintptr_t bar0_phys;
    struct e1000_rx_desc *rx_descs;
    struct e1000_tx_desc *tx_descs;
    uintptr_t rx_descs_phys;
    uintptr_t tx_descs_phys;
    void *rx_buffers[E1000_RX_DESC_COUNT];
    void *tx_buffers[E1000_TX_DESC_COUNT];
    uint32_t rx_read;
    uint32_t tx_tail;
    mutex_t rx_lock;
    mutex_t tx_lock;
    int initialized;
};

static struct e1000_state e1000;

static uintptr_t align_up_uintptr(uintptr_t value, uintptr_t alignment)
{
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static void e1000_memset(void *dest, uint8_t value, size_t count)
{
    uint8_t *bytes = dest;

    for (size_t i = 0; i < count; i++) {
        bytes[i] = value;
    }
}

static void e1000_memcpy(void *dest, const void *src, size_t count)
{
    uint8_t *out = dest;
    const uint8_t *in = src;

    for (size_t i = 0; i < count; i++) {
        out[i] = in[i];
    }
}

void e1000_write_reg(uint32_t reg, uint32_t val)
{
    if (e1000.mmio == 0) {
        return;
    }

    *(volatile uint32_t *)(e1000.mmio + reg) = val;
}

uint32_t e1000_read_reg(uint32_t reg)
{
    if (e1000.mmio == 0) {
        return 0;
    }

    return *(volatile uint32_t *)(e1000.mmio + reg);
}

static void e1000_map_mmio(uint32_t bar0_address)
{
    uintptr_t bar0 = (uintptr_t)bar0_address;
    uintptr_t phys_base = bar0 & ~(PMM_PAGE_SIZE - 1ULL);
    uintptr_t offset = bar0 & (PMM_PAGE_SIZE - 1ULL);
    uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE;

    for (uintptr_t page = 0; page < E1000_MMIO_SIZE; page += PMM_PAGE_SIZE) {
        vmm_map(E1000_MMIO_VADDR + page, phys_base + page, flags);
    }

    e1000.mmio = (volatile uint8_t *)(E1000_MMIO_VADDR + offset);
    e1000.bar0_phys = bar0;
}

static void *e1000_alloc_descriptor_page(uintptr_t virtual_alias,
    uintptr_t *physical_out)
{
    void *raw = kmalloc((size_t)(PMM_PAGE_SIZE * 2ULL));
    if (raw == 0) {
        return 0;
    }

    uintptr_t aligned = align_up_uintptr((uintptr_t)raw, PMM_PAGE_SIZE);
    uintptr_t physical = vmm_virt_to_phys(aligned);
    if (physical == 0) {
        return 0;
    }

    vmm_map(virtual_alias, physical, PAGE_PRESENT | PAGE_WRITABLE);
    *physical_out = physical;
    return (void *)virtual_alias;
}

static int e1000_alloc_rings(void)
{
    e1000.rx_descs = e1000_alloc_descriptor_page(E1000_RX_RING_VADDR,
        &e1000.rx_descs_phys);
    if (e1000.rx_descs == 0) {
        klog("e1000: falha ao alocar RX ring.\n");
        return -1;
    }

    e1000.tx_descs = e1000_alloc_descriptor_page(E1000_TX_RING_VADDR,
        &e1000.tx_descs_phys);
    if (e1000.tx_descs == 0) {
        klog("e1000: falha ao alocar TX ring.\n");
        return -1;
    }

    e1000_memset(e1000.rx_descs, 0,
        sizeof(struct e1000_rx_desc) * E1000_RX_DESC_COUNT);
    e1000_memset(e1000.tx_descs, 0,
        sizeof(struct e1000_tx_desc) * E1000_TX_DESC_COUNT);

    return 0;
}

static int e1000_alloc_packet_buffers(void)
{
    for (uint32_t i = 0; i < E1000_RX_DESC_COUNT; i++) {
        void *buffer = pmm_alloc();
        if (buffer == 0) {
            klog("e1000: falha ao alocar buffer RX.\n");
            return -1;
        }

        e1000.rx_buffers[i] = buffer;
        e1000.rx_descs[i].addr = (uint64_t)(uintptr_t)buffer;
    }

    for (uint32_t i = 0; i < E1000_TX_DESC_COUNT; i++) {
        void *buffer = pmm_alloc();
        if (buffer == 0) {
            klog("e1000: falha ao alocar buffer TX.\n");
            return -1;
        }

        e1000.tx_buffers[i] = buffer;
        e1000.tx_descs[i].addr = (uint64_t)(uintptr_t)buffer;
        e1000.tx_descs[i].status = E1000_TX_STATUS_DD;
    }

    return 0;
}

static void e1000_configure_rx(void)
{
    uint32_t ring_bytes =
        sizeof(struct e1000_rx_desc) * E1000_RX_DESC_COUNT;

    e1000_write_reg(E1000_REG_RDBAL, (uint32_t)e1000.rx_descs_phys);
    e1000_write_reg(E1000_REG_RDBAH, (uint32_t)(e1000.rx_descs_phys >> 32));
    e1000_write_reg(E1000_REG_RDLEN, ring_bytes);
    e1000_write_reg(E1000_REG_RDH, 0);
    e1000_write_reg(E1000_REG_RDT, E1000_RX_DESC_COUNT - 1U);
    e1000.rx_read = 0;

    e1000_write_reg(E1000_REG_RCTL,
        E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
}

static void e1000_configure_tx(void)
{
    uint32_t ring_bytes =
        sizeof(struct e1000_tx_desc) * E1000_TX_DESC_COUNT;

    e1000_write_reg(E1000_REG_TDBAL, (uint32_t)e1000.tx_descs_phys);
    e1000_write_reg(E1000_REG_TDBAH, (uint32_t)(e1000.tx_descs_phys >> 32));
    e1000_write_reg(E1000_REG_TDLEN, ring_bytes);
    e1000_write_reg(E1000_REG_TDH, 0);
    e1000_write_reg(E1000_REG_TDT, 0);
    e1000.tx_tail = 0;

    e1000_write_reg(E1000_REG_TCTL,
        E1000_TCTL_EN |
        E1000_TCTL_PSP |
        (0x10U << E1000_TCTL_CT_SHIFT) |
        (0x40U << E1000_TCTL_COLD_SHIFT));
    e1000_write_reg(E1000_REG_TIPG, 10U | (8U << 10) | (6U << 20));
}

int e1000_init(uint32_t bar0_address)
{
    if (e1000.initialized) {
        return 0;
    }

    e1000_memset(&e1000, 0, sizeof(e1000));
    mutex_init(&e1000.rx_lock);
    mutex_init(&e1000.tx_lock);

    e1000_map_mmio(bar0_address);
    (void)e1000_read_reg(E1000_REG_STATUS);
    e1000_write_reg(E1000_REG_IMC, 0xFFFFFFFFU);

    if (e1000_alloc_rings() != 0) {
        return -1;
    }

    if (e1000_alloc_packet_buffers() != 0) {
        return -1;
    }

    e1000_configure_rx();
    e1000_configure_tx();

    e1000.initialized = 1;
    klog("e1000: controlador inicializado.\n");
    return 0;
}

int e1000_send_packet(const void *packet, size_t length)
{
    if (!e1000.initialized || packet == 0 ||
        length == 0 || length > E1000_MAX_PACKET_SIZE) {
        return -1;
    }

    mutex_lock(&e1000.tx_lock);

    uint32_t tail = e1000_read_reg(E1000_REG_TDT) % E1000_TX_DESC_COUNT;
    volatile struct e1000_tx_desc *desc = &e1000.tx_descs[tail];
    if ((desc->status & E1000_TX_STATUS_DD) == 0) {
        mutex_unlock(&e1000.tx_lock);
        return 0;
    }

    e1000_memcpy(e1000.tx_buffers[tail], packet, length);
    desc->length = (uint16_t)length;
    desc->cso = 0;
    desc->cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    tail = (tail + 1U) % E1000_TX_DESC_COUNT;
    e1000.tx_tail = tail;
    e1000_write_reg(E1000_REG_TDT, tail);

    for (uint32_t spin = 0; spin < E1000_TX_POLL_LIMIT; spin++) {
        if ((desc->status & E1000_TX_STATUS_DD) != 0) {
            mutex_unlock(&e1000.tx_lock);
            return (int)length;
        }

        __asm__ volatile ("pause");
    }

    mutex_unlock(&e1000.tx_lock);
    return -1;
}

int e1000_receive_packet(void *buffer_out, size_t max_length)
{
    if (!e1000.initialized || buffer_out == 0 || max_length == 0) {
        return -1;
    }

    mutex_lock(&e1000.rx_lock);

    uint32_t index = e1000.rx_read;
    volatile struct e1000_rx_desc *desc = &e1000.rx_descs[index];
    if ((desc->status & E1000_RX_STATUS_DD) == 0) {
        mutex_unlock(&e1000.rx_lock);
        return 0;
    }

    size_t length = desc->length;
    int result = -1;
    if (length <= E1000_PACKET_BUFFER_SIZE && length <= max_length) {
        e1000_memcpy(buffer_out, e1000.rx_buffers[index], length);
        result = (int)length;
    }

    desc->status = 0;
    e1000_write_reg(E1000_REG_RDT, index);
    e1000.rx_read = (index + 1U) % E1000_RX_DESC_COUNT;

    mutex_unlock(&e1000.rx_lock);
    return result;
}
