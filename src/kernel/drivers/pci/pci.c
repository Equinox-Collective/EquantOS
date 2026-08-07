// pci.c - PCI Bus enumeration and MMIO mapping implementation
#include "pci.h"
#include "../../core/mem/vmm.h"
#include "../serial/serial.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_REG_VENDOR_DEVICE  0x00
#define PCI_REG_COMMAND        0x04
#define PCI_REG_REVISION_CLASS 0x08
#define PCI_REG_HEADER_TYPE    0x0C
#define PCI_REG_BAR0           0x10
#define PCI_REG_BAR4           0x20

#define PCI_CMD_IO_SPACE       (1 << 0)
#define PCI_CMD_MEM_SPACE      (1 << 1)
#define PCI_CMD_BUS_MASTER     (1 << 2)

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
        // Map with Page-Level Cache Disable (PCD) and Write-Through (PWT) for MMIO safety
        vmm_map(pml4, virt_start + (i * PAGE_SIZE), (phys_addr & ~0xFFFULL) + (i * PAGE_SIZE),
                PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    }

    return (void *)(virt_start + (phys_addr & 0xFFF));
}

static void pci_check_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t vendor_device = pci_read_dword(bus, slot, func, PCI_REG_VENDOR_DEVICE);
    uint16_t vendor = vendor_device & 0xFFFF;
    uint16_t device = (vendor_device >> 16) & 0xFFFF;

    if (vendor == 0xFFFF) return; // Non-existent device

    uint32_t class_rev = pci_read_dword(bus, slot, func, PCI_REG_REVISION_CLASS);
    uint8_t class_code = (class_rev >> 24) & 0xFF;
    uint8_t subclass   = (class_rev >> 16) & 0xFF;

    // Log discovered PCI devices to serial port for diagnostics
    serial_puts(COM1, "[PCI] Found device\n");

    // Stub hook for future driver hooks (e.g., USB, NIC, Storage controllers)
    (void)class_code;
    (void)subclass;
}

static void pci_check_device(uint8_t bus, uint8_t slot) {
    uint32_t vendor_device = pci_read_dword(bus, slot, 0, PCI_REG_VENDOR_DEVICE);
    uint16_t vendor = vendor_device & 0xFFFF;

    if (vendor == 0xFFFF) return; 

    uint32_t header_reg = pci_read_dword(bus, slot, 0, PCI_REG_HEADER_TYPE);
    uint8_t header_type = (header_reg >> 16) & 0xFF;

    if (header_type & 0x80) {
        // Multi-function device, check all 8 functions
        for (uint8_t func = 0; func < 8; func++) {
            pci_check_function(bus, slot, func);
        }
    } else {
        // Single-function device
        pci_check_function(bus, slot, 0);
    }
}

void pci_init(void) {
    serial_puts(COM1, "[PCI] Scanning PCI buses...\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            pci_check_device(bus, slot);
        }
    }
    serial_puts(COM1, "[PCI] Bus scan complete.\n");
}