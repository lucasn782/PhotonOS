#ifndef PHOTONOS_PCI_H
#define PHOTONOS_PCI_H

#include <stdint.h>

uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func,
    uint8_t offset);
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func,
    uint8_t offset, uint32_t value);
int pci_init(void);

#endif
