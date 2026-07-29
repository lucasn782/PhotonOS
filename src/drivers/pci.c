#include "pci.h"

#include "e1000.h"
#include "serial.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_ENABLE_BIT 0x80000000U

#define PCI_VENDOR_INVALID 0xFFFFU
#define PCI_E1000_VENDOR_ID 0x8086U
#define PCI_E1000_DEVICE_ID_82540EM 0x100EU
#define PCI_E1000_DEVICE_ID_82545EM 0x100FU
#define PCI_RTL8139_VENDOR_ID 0x10ECU
#define PCI_RTL8139_DEVICE_ID 0x8139U

#define PCI_COMMAND_OFFSET 0x04
#define PCI_CLASS_OFFSET 0x08
#define PCI_BAR0_OFFSET 0x10
#define PCI_BAR1_OFFSET 0x14

#define PCI_CLASS_NETWORK 0x02U
#define PCI_SUBCLASS_ETHERNET 0x00U

#define PCI_COMMAND_IO_SPACE (1U << 0)
#define PCI_COMMAND_MEMORY_SPACE (1U << 1)
#define PCI_COMMAND_BUS_MASTER (1U << 2)

#define PCI_BAR_IO_SPACE 0x1U
#define PCI_BAR_IO_MASK 0xFFFFFFFCU
#define PCI_BAR_MEM_MASK 0xFFFFFFF0U
#define PCI_BAR_MEM_TYPE_MASK 0x6U
#define PCI_BAR_MEM_TYPE_64 0x4U

struct pci_bar {
    uint64_t address;
    uint32_t raw;
    uint8_t valid;
    uint8_t is_io;
    uint8_t is_64;
};

/* Keep driver state out of the legacy VGA aperture used by late .bss. */
static uint32_t pci_ethernet_seen __attribute__((section(".network_state")));
static uint32_t pci_nic_initialized __attribute__((section(".network_state")));
static uint32_t pci_unsupported_logged __attribute__((section(".network_state")));

static uint32_t inl(uint16_t port)
{
    uint32_t value;

    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func,
    uint8_t offset)
{
    uint32_t address = PCI_ENABLE_BIT |
        ((uint32_t)bus << 16) |
        ((uint32_t)slot << 11) |
        ((uint32_t)func << 8) |
        (offset & 0xFCU);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func,
    uint8_t offset, uint32_t value)
{
    uint32_t address = PCI_ENABLE_BIT |
        ((uint32_t)bus << 16) |
        ((uint32_t)slot << 11) |
        ((uint32_t)func << 8) |
        (offset & 0xFCU);

    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static int pci_is_supported_e1000(uint16_t vendor, uint16_t device)
{
    if (vendor != PCI_E1000_VENDOR_ID) {
        return 0;
    }

    return device == PCI_E1000_DEVICE_ID_82540EM ||
        device == PCI_E1000_DEVICE_ID_82545EM;
}

static struct pci_bar pci_read_bar(uint8_t bus, uint8_t slot, uint8_t func,
    uint8_t bar_offset)
{
    struct pci_bar bar;
    bar.address = 0;
    bar.raw = pci_read_config(bus, slot, func, bar_offset);
    bar.valid = 0;
    bar.is_io = 0;
    bar.is_64 = 0;

    if (bar.raw == 0 || bar.raw == 0xFFFFFFFFU) {
        return bar;
    }

    if ((bar.raw & PCI_BAR_IO_SPACE) != 0) {
        bar.address = (uint64_t)(bar.raw & PCI_BAR_IO_MASK);
        bar.is_io = 1;
        bar.valid = bar.address != 0;
        return bar;
    }

    bar.address = (uint64_t)(bar.raw & PCI_BAR_MEM_MASK);
    if ((bar.raw & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64) {
        uint32_t high = pci_read_config(bus, slot, func, bar_offset + 4U);
        bar.address |= (uint64_t)high << 32;
        bar.is_64 = 1;
    }
    bar.valid = bar.address != 0;
    return bar;
}

static void pci_enable_device(uint8_t bus, uint8_t slot, uint8_t func,
    int enable_io, int enable_memory)
{
    uint32_t command = pci_read_config(bus, slot, func, PCI_COMMAND_OFFSET) &
        0xFFFFU;

    command |= PCI_COMMAND_BUS_MASTER;
    if (enable_io) {
        command |= PCI_COMMAND_IO_SPACE;
    }
    if (enable_memory) {
        command |= PCI_COMMAND_MEMORY_SPACE;
    }

    pci_write_config(bus, slot, func, PCI_COMMAND_OFFSET, command);
}

static void pci_log_unsupported_ethernet(uint16_t vendor, uint16_t device)
{
    if (pci_unsupported_logged != 0) {
        return;
    }

    pci_unsupported_logged = 1;
    if (vendor == PCI_RTL8139_VENDOR_ID &&
        device == PCI_RTL8139_DEVICE_ID) {
        klog("PCI: controlador Ethernet RTL8139 detectado sem driver ativo.\n");
    } else {
        klog("PCI: controlador Ethernet sem driver suportado.\n");
    }
}

static void pci_probe_ethernet(uint8_t bus, uint8_t slot, uint8_t func,
    uint16_t vendor, uint16_t device)
{
    struct pci_bar bar0;
    struct pci_bar bar1;
    struct pci_bar mmio_bar;

    pci_ethernet_seen = 1;

    if (pci_nic_initialized != 0) {
        return;
    }

    if (!pci_is_supported_e1000(vendor, device)) {
        pci_log_unsupported_ethernet(vendor, device);
        return;
    }

    bar0 = pci_read_bar(bus, slot, func, PCI_BAR0_OFFSET);
    bar1 = bar0.is_64 ? (struct pci_bar){0, 0, 0, 0, 0} :
        pci_read_bar(bus, slot, func, PCI_BAR1_OFFSET);
    mmio_bar = bar0.valid && !bar0.is_io ? bar0 : bar1;

    if (!mmio_bar.valid || mmio_bar.is_io) {
        klog("PCI: Intel e1000 sem BAR MMIO utilizavel.\n");
        return;
    }

    pci_enable_device(bus, slot, func, 0, 1);

    if (e1000_init(mmio_bar.address) == 0) {
        pci_nic_initialized = 1;
        klog("PCI: controlador Ethernet e1000 inicializado.\n");
    } else {
        klog("PCI: falha ao inicializar controlador Ethernet e1000.\n");
    }
}

static void pci_probe_function(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t id = pci_read_config(bus, slot, func, 0x00);
    uint16_t vendor = (uint16_t)(id & 0xFFFFU);
    uint16_t device = (uint16_t)((id >> 16) & 0xFFFFU);
    uint32_t class_info;
    uint8_t class_code;
    uint8_t subclass;

    if (vendor == PCI_VENDOR_INVALID) {
        return;
    }

    class_info = pci_read_config(bus, slot, func, PCI_CLASS_OFFSET);
    class_code = (uint8_t)((class_info >> 24) & 0xFFU);
    subclass = (uint8_t)((class_info >> 16) & 0xFFU);

    if (class_code == PCI_CLASS_NETWORK &&
        subclass == PCI_SUBCLASS_ETHERNET) {
        pci_probe_ethernet(bus, slot, func, vendor, device);
    }
}

int pci_init(void)
{
    e1000_reset();
    pci_ethernet_seen = 0;
    pci_nic_initialized = 0;
    pci_unsupported_logged = 0;

    klog("PCI: iniciando varredura completa do barramento.\n");

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                pci_probe_function((uint8_t)bus, slot, func);
            }
        }
    }

    if (pci_nic_initialized != 0) {
        return 0;
    }

    if (pci_ethernet_seen == 0) {
        klog("PCI: nenhum controlador Ethernet encontrado.\n");
    } else {
        klog("PCI: nenhum controlador Ethernet suportado inicializado.\n");
    }

    return -1;
}
