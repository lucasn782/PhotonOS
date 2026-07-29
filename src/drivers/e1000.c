#include "e1000.h"

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
#define E1000_RX_BUFFER_VADDR 0x00000000F0030000ULL
#define E1000_TX_BUFFER_VADDR \
    (E1000_RX_BUFFER_VADDR + (E1000_RX_DESC_COUNT * PMM_PAGE_SIZE))

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
    uintptr_t bar_phys;
    struct e1000_rx_desc *rx_descs;
    struct e1000_tx_desc *tx_descs;
    uintptr_t rx_descs_phys;
    uintptr_t tx_descs_phys;
    void *rx_buffers[E1000_RX_DESC_COUNT];
    void *tx_buffers[E1000_TX_DESC_COUNT];
    uintptr_t rx_buffer_phys[E1000_RX_DESC_COUNT];
    uintptr_t tx_buffer_phys[E1000_TX_DESC_COUNT];
    uint32_t rx_read;
    uint32_t tx_tail;
    mutex_t rx_lock;
    mutex_t tx_lock;
    int initialized;
};

/* Must remain outside the 0xA0000-0xBFFFF legacy VGA aperture. */
static struct e1000_state e1000 __attribute__((section(".network_state")));

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

    volatile uint32_t *mmio_reg = (volatile uint32_t *)(e1000.mmio + reg);
    *mmio_reg = val;
}

uint32_t e1000_read_reg(uint32_t reg)
{
    if (e1000.mmio == 0) {
        return 0;
    }

    volatile uint32_t *mmio_reg = (volatile uint32_t *)(e1000.mmio + reg);
    return *mmio_reg;
}

static inline void e1000_memory_barrier(void)
{
    __asm__ volatile("" : : : "memory");
}

static void e1000_write_tail(uint32_t reg, uint32_t val)
{
    e1000_memory_barrier();
    e1000_write_reg(reg, val);
    e1000_memory_barrier();
}

static uint64_t save_and_disable_interrupts(void)
{
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    __asm__ volatile ("cli" ::: "memory");
    return rflags;
}

static void restore_interrupts(uint64_t rflags)
{
    if ((rflags & 0x200ULL) != 0) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

static void e1000_map_mmio(uint64_t bar_address)
{
    uintptr_t bar = (uintptr_t)bar_address;
    uintptr_t phys_base = bar & ~(PMM_PAGE_SIZE - 1ULL);
    uintptr_t offset = bar & (PMM_PAGE_SIZE - 1ULL);
    uint32_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE;

    for (uintptr_t page = 0; page < E1000_MMIO_SIZE; page += PMM_PAGE_SIZE) {
        vmm_map(E1000_MMIO_VADDR + page, phys_base + page, flags);
    }

    e1000.mmio = (volatile uint8_t *)(E1000_MMIO_VADDR + offset);
    e1000.bar_phys = bar;
}

static int e1000_dma_address_from_page(void *page, uintptr_t *physical_out)
{
    uintptr_t address = (uintptr_t)page;
    uintptr_t physical = address;

    if (address >= PMM_TOTAL_MEMORY) {
        physical = vmm_virt_to_phys(address);
    }

    if (physical == 0 || physical >= PMM_TOTAL_MEMORY ||
        (physical & (PMM_PAGE_SIZE - 1ULL)) != 0) {
        return -1;
    }

    *physical_out = physical;
    return 0;
}

static int e1000_alloc_dma_page(uintptr_t virtual_alias, void **virtual_out,
    uintptr_t *physical_out)
{
    void *physical_page = pmm_alloc();
    if (physical_page == 0) {
        return -1;
    }

    uintptr_t physical;
    if (e1000_dma_address_from_page(physical_page, &physical) != 0) {
        klog("e1000: pagina DMA invalida.\n");
        return -1;
    }

    vmm_map(virtual_alias, physical, PAGE_PRESENT | PAGE_WRITABLE);
    *physical_out = physical;
    *virtual_out = (void *)virtual_alias;
    return 0;
}

static int e1000_alloc_rings(void)
{
    if (e1000_alloc_dma_page(E1000_RX_RING_VADDR,
        (void **)&e1000.rx_descs, &e1000.rx_descs_phys) != 0) {
        klog("e1000: falha ao alocar RX ring.\n");
        return -1;
    }

    if (e1000_alloc_dma_page(E1000_TX_RING_VADDR,
        (void **)&e1000.tx_descs, &e1000.tx_descs_phys) != 0) {
        klog("e1000: falha ao alocar TX ring.\n");
        return -1;
    }

    e1000_memset(e1000.rx_descs, 0,
        sizeof(struct e1000_rx_desc) * E1000_RX_DESC_COUNT);
    e1000_memset(e1000.tx_descs, 0,
        sizeof(struct e1000_tx_desc) * E1000_TX_DESC_COUNT);

    return 0;
}

static uintptr_t e1000_rx_buffer_vaddr(uint32_t index)
{
    return E1000_RX_BUFFER_VADDR + ((uintptr_t)index * PMM_PAGE_SIZE);
}

static uintptr_t e1000_tx_buffer_vaddr(uint32_t index)
{
    return E1000_TX_BUFFER_VADDR + ((uintptr_t)index * PMM_PAGE_SIZE);
}

static int e1000_alloc_packet_buffers(void)
{
    for (uint32_t i = 0; i < E1000_RX_DESC_COUNT; i++) {
        uintptr_t phys;
        void *buffer;

        if (e1000_alloc_dma_page(e1000_rx_buffer_vaddr(i), &buffer,
            &phys) != 0) {
            klog("e1000: falha ao alocar buffer RX.\n");
            return -1;
        }

        e1000.rx_buffers[i] = buffer;
        e1000.rx_buffer_phys[i] = phys;
        e1000.rx_descs[i].addr = (uint64_t)phys;
    }

    for (uint32_t i = 0; i < E1000_TX_DESC_COUNT; i++) {
        uintptr_t phys;
        void *buffer;

        if (e1000_alloc_dma_page(e1000_tx_buffer_vaddr(i), &buffer,
            &phys) != 0) {
            klog("e1000: falha ao alocar buffer TX.\n");
            return -1;
        }

        e1000.tx_buffers[i] = buffer;
        e1000.tx_buffer_phys[i] = phys;
        e1000.tx_descs[i].addr = (uint64_t)phys;
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
    e1000_write_tail(E1000_REG_RDT, E1000_RX_DESC_COUNT - 1U);
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
    e1000_write_tail(E1000_REG_TDT, 0);
    e1000.tx_tail = 0;

    e1000_write_reg(E1000_REG_TCTL,
        E1000_TCTL_EN |
        E1000_TCTL_PSP |
        (0x10U << E1000_TCTL_CT_SHIFT) |
        (0x40U << E1000_TCTL_COLD_SHIFT));
    e1000_write_reg(E1000_REG_TIPG, 10U | (8U << 10) | (6U << 20));
}

int e1000_init(uint64_t bar_address)
{
    if (e1000.initialized) {
        return 0;
    }

    e1000_memset(&e1000, 0, sizeof(e1000));
    mutex_init(&e1000.rx_lock);
    mutex_init(&e1000.tx_lock);

    e1000_map_mmio(bar_address);
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

void e1000_reset(void)
{
    e1000_memset(&e1000, 0, sizeof(e1000));
    mutex_init(&e1000.rx_lock);
    mutex_init(&e1000.tx_lock);
}

int e1000_send_packet(const void *packet, size_t length)
{
    if (!e1000.initialized || packet == 0 ||
        length == 0 || length > E1000_MAX_PACKET_SIZE) {
        return -1;
    }

    uint64_t rflags = save_and_disable_interrupts();
    mutex_lock(&e1000.tx_lock);

    uint32_t tail = e1000_read_reg(E1000_REG_TDT) % E1000_TX_DESC_COUNT;
    volatile struct e1000_tx_desc *desc = &e1000.tx_descs[tail];
    if ((desc->status & E1000_TX_STATUS_DD) == 0) {
        mutex_unlock(&e1000.tx_lock);
        restore_interrupts(rflags);
        return 0;
    }

    e1000_memcpy(e1000.tx_buffers[tail], packet, length);
    desc->addr = (uint64_t)e1000.tx_buffer_phys[tail];
    desc->length = (uint16_t)length;
    desc->cso = 0;
    desc->cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->status = 0;
    desc->css = 0;
    desc->special = 0;

    tail = (tail + 1U) % E1000_TX_DESC_COUNT;
    e1000.tx_tail = tail;
    e1000_write_tail(E1000_REG_TDT, tail);

    mutex_unlock(&e1000.tx_lock);
    restore_interrupts(rflags);
    return (int)length;
}

int e1000_receive_packet(void *buffer_out, size_t max_length)
{
    if (!e1000.initialized || buffer_out == 0 || max_length == 0) {
        return -1;
    }

    uint64_t rflags = save_and_disable_interrupts();
    mutex_lock(&e1000.rx_lock);

    uint32_t index = e1000.rx_read;
    volatile struct e1000_rx_desc *desc = &e1000.rx_descs[index];
    if ((desc->status & E1000_RX_STATUS_DD) == 0) {
        mutex_unlock(&e1000.rx_lock);
        restore_interrupts(rflags);
        return 0;
    }

    size_t length = desc->length;
    int result = -1;
    if (length <= E1000_PACKET_BUFFER_SIZE && length <= max_length) {
        e1000_memcpy(buffer_out, e1000.rx_buffers[index], length);
        result = (int)length;
    }

    desc->addr = (uint64_t)e1000.rx_buffer_phys[index];
    desc->length = 0;
    desc->checksum = 0;
    desc->errors = 0;
    desc->special = 0;
    desc->status = 0;
    e1000_write_tail(E1000_REG_RDT, index);
    e1000.rx_read = (index + 1U) % E1000_RX_DESC_COUNT;

    mutex_unlock(&e1000.rx_lock);
    restore_interrupts(rflags);
    return result;
}
