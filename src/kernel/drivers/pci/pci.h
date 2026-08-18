// src/kernel/drivers/pci/pci.h - PCI Express & Legacy Driver Infrastructure
#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
} pci_device_t;

typedef struct {
    uint16_t vendor_id; // 0xFFFF = ANY
    uint16_t device_id; // 0xFFFF = ANY
    uint8_t  class_code;
    uint8_t  subclass;
} pci_device_id_t;

typedef struct pci_driver {
    const char *name;
    pci_device_id_t *id_table;
    int (*probe)(pci_device_t *dev);
    void (*remove)(pci_device_t *dev);
    struct pci_driver *next;
} pci_driver_t;

void pci_init(void);
uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);

void *pci_map_mmio(uint64_t phys_addr, uint32_t size);
void pci_register_driver(pci_driver_t *driver);

#endif // PCI_H