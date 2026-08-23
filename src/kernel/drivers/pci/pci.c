// src/kernel/drivers/pci/pci.c - Fully Documented PCI Bus Core
#include "pci.h"
#include "../../core/mem/vmm.h"
#include "../serial/serial.h"
#include "../../core/initcall.h"
#include "stdio.h"
#include <stddef.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define MAX_PCI_DEVICES    64

static pci_driver_t *registered_drivers = NULL;
static pci_device_t pci_devices[MAX_PCI_DEVICES];
static size_t pci_device_count = 0;
static bool pci_bus_scanned = false;

uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((1U << 31) | 
                                  ((bus & 0xFF) << 16) | 
                                  ((slot & 0x1F) << 11) | 
                                  ((func & 0x07) << 8) | 
                                  (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t address = (uint32_t)((1U << 31) | 
                                  ((bus & 0xFF) << 16) | 
                                  ((slot & 0x1F) << 11) | 
                                  ((func & 0x07) << 8) | 
                                  (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    outw(PCI_CONFIG_DATA + (offset & 2), val);
}

void *pci_map_mmio(uint64_t phys_addr, uint32_t size) {
    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    page_table_t *pml4 = (page_table_t *)VIRT(cr3_val & ~0xFFFULL);

    static uint64_t mmio_virt_ptr = 0xFFFFC20000000000;
    uint64_t virt_start = mmio_virt_ptr;
    
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    mmio_virt_ptr += (pages * PAGE_SIZE);

    for (uint32_t i = 0; i < pages; i++) {
        vmm_map(pml4, virt_start + (i * PAGE_SIZE), (phys_addr & ~0xFFFULL) + (i * PAGE_SIZE),
                PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    }

    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), "[PCI-MMIO] Mapped Phys 0x%lx -> Virt 0x%lx (Size: %u bytes)\n",
             phys_addr, virt_start, size);
    serial_puts(COM1, log_buf);

    return (void *)(virt_start + (phys_addr & 0xFFF));
}

static void pci_match_device_against_driver(pci_device_t *dev, pci_driver_t *drv) {
    if (!dev || !drv || !drv->id_table) return;

    pci_device_id_t *id = drv->id_table;
    while (id->vendor_id != 0 || id->class_code != 0) {
        bool match_vendor = (id->vendor_id == 0xFFFF || id->vendor_id == dev->vendor_id);
        bool match_device = (id->device_id == 0xFFFF || id->device_id == dev->device_id);
        bool match_class  = (id->class_code == 0 || id->class_code == dev->class_code);
        bool match_sub    = (id->subclass == 0 || id->subclass == dev->subclass);

        if (match_vendor && match_device && match_class && match_sub) {
            char log_buf[128];
            snprintf(log_buf, sizeof(log_buf), 
                     "[PCI-MATCH] Device %02x:%02x.%d matched Driver: '%s'\n",
                     dev->bus, dev->slot, dev->func, drv->name);
            serial_puts(COM1, log_buf);

            if (drv->probe) {
                drv->probe(dev);
            }
            return;
        }
        id++;
    }
}

void pci_register_driver(pci_driver_t *driver) {
    if (!driver) return;
    
    driver->next = registered_drivers;
    registered_drivers = driver;

    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), "[PCI-CORE] Registered Driver: '%s'\n", driver->name);
    serial_puts(COM1, log_buf);

    if (pci_bus_scanned) {
        for (size_t i = 0; i < pci_device_count; i++) {
            pci_match_device_against_driver(&pci_devices[i], driver);
        }
    }
}

static void pci_check_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t vendor_device = pci_read_dword(bus, slot, func, 0x00);
    uint16_t vendor = vendor_device & 0xFFFF;
    uint16_t device = (vendor_device >> 16) & 0xFFFF;

    if (vendor == 0xFFFF) return;

    uint32_t class_rev = pci_read_dword(bus, slot, func, 0x08);
    
    if (pci_device_count < MAX_PCI_DEVICES) {
        pci_device_t *dev = &pci_devices[pci_device_count++];
        dev->bus = bus;
        dev->slot = slot;
        dev->func = func;
        dev->vendor_id = vendor;
        dev->device_id = device;
        dev->class_code = (class_rev >> 24) & 0xFF;
        dev->subclass   = (class_rev >> 16) & 0xFF;
        dev->prog_if    = (class_rev >> 8)  & 0xFF;
        dev->revision   = class_rev & 0xFF;

        char log_buf[128];
        snprintf(log_buf, sizeof(log_buf), 
                 "[PCI-DEV] Found %02x:%02x.%d | Vendor: 0x%04x | Device: 0x%04x | Class: 0x%02x Sub: 0x%02x ProgIF: 0x%02x\n",
                 bus, slot, func, vendor, device, dev->class_code, dev->subclass, dev->prog_if);
        serial_puts(COM1, log_buf);

        pci_driver_t *drv = registered_drivers;
        while (drv) {
            pci_match_device_against_driver(dev, drv);
            drv = drv->next;
        }
    }
}

static void pci_check_device(uint8_t bus, uint8_t slot) {
    uint32_t vendor_device = pci_read_dword(bus, slot, 0, 0x00);
    if ((vendor_device & 0xFFFF) == 0xFFFF) return;

    uint32_t header_reg = pci_read_dword(bus, slot, 0, 0x0C);
    uint8_t header_type = (header_reg >> 16) & 0xFF;

    if (header_type & 0x80) {
        for (uint8_t func = 0; func < 8; func++) {
            pci_check_function(bus, slot, func);
        }
    } else {
        pci_check_function(bus, slot, 0);
    }
}

void pci_init(void) {
    if (pci_bus_scanned) return;
    
    serial_puts(COM1, "[PCI-CORE] Enumerating PCI bus topology...\n");
    pci_device_count = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            pci_check_device(bus, slot);
        }
    }

    pci_bus_scanned = true;
    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), "[PCI-CORE] Scan complete. Total cached devices: %u\n", (unsigned int)pci_device_count);
    serial_puts(COM1, log_buf);
}

static int __init pci_subsys_init(void) {
    pci_init();
    return 0;
}

subsys_initcall(pci_subsys_init);