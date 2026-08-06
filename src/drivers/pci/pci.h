// pci.h - PCI Bus Driver Interface
#ifndef PCI_H
#define PCI_H

#include <stdint.h>

void pci_init(void);
uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);

// Maps PCI Memory-Mapped I/O physical addresses into virtual space with uncacheable flags
void *pci_map_mmio(uint64_t phys_addr, uint32_t size);

#endif // PCI_H