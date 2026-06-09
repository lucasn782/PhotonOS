#include "pci.h"

#include "e1000.h"
#include "serial.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_ENABLE_BIT 0x80000000U

#define PCI_VENDOR_INVALID 0xFFFFU
#define PCI_E1000_VENDOR_ID 0x8086U
#define PCI_E1000_DEVICE_ID 0x100EU

#define PCI_COMMAND_OFFSET 0x04
#define PCI_HEADER_TYPE_OFFSET 0x0C
#define PCI_BAR0_OFFSET 0x10

#define PCI_COMMAND_MEMORY_SPACE (1U << 1)
#define PCI_COMMAND_BUS_MASTER (1U << 2)

static uint32_t pci_device_found;

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

static void pci_enable_mmio_bus_master(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t command = pci_read_config(bus, slot, func, PCI_COMMAND_OFFSET);

    command |= PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;
    pci_write_config(bus, slot, func, PCI_COMMAND_OFFSET, command);
}

static void pci_probe_function(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t id = pci_read_config(bus, slot, func, 0x00);
    uint16_t vendor = (uint16_t)(id & 0xFFFFU);
    uint16_t device = (uint16_t)((id >> 16) & 0xFFFFU);

    if (vendor == PCI_VENDOR_INVALID) {
        return;
    }

    if (vendor != PCI_E1000_VENDOR_ID || device != PCI_E1000_DEVICE_ID) {
        return;
    }

    uint32_t bar0 = pci_read_config(bus, slot, func, PCI_BAR0_OFFSET);
    if ((bar0 & 0x1U) != 0) {
        klog("PCI: Intel e1000 BAR0 nao e MMIO.\n");
        return;
    }

    pci_enable_mmio_bus_master(bus, slot, func);

    if (e1000_init(bar0 & 0xFFFFFFF0U) == 0) {
        pci_device_found = 1;
        klog("PCI: Intel e1000 detectada via barramento PCI.\n");
    } else {
        klog("PCI: falha ao inicializar Intel e1000.\n");
    }
}

static void pci_scan_slot(uint8_t bus, uint8_t slot)
{
    uint32_t id = pci_read_config(bus, slot, 0, 0x00);
    uint16_t vendor = (uint16_t)(id & 0xFFFFU);

    if (vendor == PCI_VENDOR_INVALID) {
        return;
    }

    uint32_t header = pci_read_config(bus, slot, 0, PCI_HEADER_TYPE_OFFSET);
    uint8_t header_type = (uint8_t)((header >> 16) & 0xFFU);
    uint8_t functions = (header_type & 0x80U) != 0 ? 8 : 1;

    for (uint8_t func = 0; func < functions; func++) {
        pci_probe_function(bus, slot, func);
    }
}

static void pci_scan_bus(uint8_t bus)
{
    for (uint8_t slot = 0; slot < 32; slot++) {
        pci_scan_slot(bus, slot);
    }
}

int pci_init(void)
{
    e1000_reset();
    pci_device_found = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        pci_scan_bus((uint8_t)bus);
    }

    if (pci_device_found == 0) {
        klog("PCI: Intel e1000 nao encontrada.\n");
        return -1;
    }

    return 0;
}
