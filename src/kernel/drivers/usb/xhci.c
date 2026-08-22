// src/kernel/drivers/usb/xhci.c - USB 3.0 xHCI Controller Implementation
#include "xhci.h"
#include "../pci/pci.h"
#include "../../core/mem/pmm.h"
#include "../../core/mem/vmm.h"
#include "../../core/mem/memory.h"
#include "../serial/serial.h"
#include "../../core/initcall.h"
#include "string.h"

static xhci_t g_xhci;

static void xhci_ring_cmd(xhci_t *xhci, uint64_t param, uint32_t status, uint32_t control) {
    xhci_trb_t *trb = &xhci->cmd_ring_virt[xhci->cmd_enqueue_idx];
    trb->parameter = param;
    trb->status    = status;
    
    // Set Cycle Bit matching Producer Cycle State (PCS)
    uint32_t ctrl = (control & ~1U) | xhci->cmd_pcs;
    trb->control  = ctrl;

    xhci->cmd_enqueue_idx++;

    // Handle Ring Overlap and Link TRB
    if (xhci->cmd_enqueue_idx >= XHCI_TRB_RING_SIZE - 1) {
        xhci_trb_t *link = &xhci->cmd_ring_virt[xhci->cmd_enqueue_idx];
        link->parameter = xhci->cmd_ring_phys;
        link->status    = 0;
        link->control   = (TRB_TYPE_LINK << 10) | (1U << 1) | xhci->cmd_pcs; // Toggle Cycle Flag
        
        xhci->cmd_enqueue_idx = 0;
        xhci->cmd_pcs ^= 1;
    }

    // Ring Doorbell 0 for Host Controller Command Execution
    xhci->db_regs[0] = 0;
}

static int xhci_probe(pci_device_t *dev) {
    serial_puts(COM1, "[xHCI] Initializing USB 3.0 Host Controller...\n");

    // 1. Enable MMIO Space & Bus Master in PCI Command Register
    pci_write_word(dev->bus, dev->slot, dev->func, 0x04, 0x0006);

    // 2. Read BAR0 Base MMIO Physical Address
    uint32_t bar0_low  = pci_read_dword(dev->bus, dev->slot, dev->func, 0x10);
    uint32_t bar0_high = pci_read_dword(dev->bus, dev->slot, dev->func, 0x14);
    uint64_t bar0 = ((uint64_t)bar0_high << 32) | (bar0_low & ~0x0FULL);

    if (!bar0) {
        serial_puts(COM1, "[xHCI] Error: Invalid BAR0 MMIO Address!\n");
        return -1;
    }

    // 3. Map xHCI MMIO Registers into Virtual Address Space
    g_xhci.cap_regs = (volatile xhci_cap_regs_t *)pci_map_mmio(bar0, 0x10000);
    g_xhci.op_regs  = (volatile xhci_op_regs_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->caplength);
    g_xhci.rt_regs  = (volatile xhci_rt_regs_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->rtoff);
    g_xhci.db_regs  = (volatile uint32_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->dboff);

    // 4. Halt Host Controller before reset
    g_xhci.op_regs->usbcmd &= ~1U; // Clear RS (Run/Stop) bit
    while (!(g_xhci.op_regs->usbsts & 1U)) {
        // Wait for HCHalted == 1
    }

    // 5. Reset Host Controller
    g_xhci.op_regs->usbcmd |= (1U << 1); // Set HCRST (Host Controller Reset)
    while (g_xhci.op_regs->usbcmd & (1U << 1)) {
        // Wait until HCRST clears
    }
    while (g_xhci.op_regs->usbsts & (1U << 11)) {
        // Wait until CNR (Controller Not Ready) clears
    }

    // 6. Inspect Capabilities
    g_xhci.max_slots = g_xhci.cap_regs->hcsparams1 & 0xFF;
    g_xhci.max_ports = (g_xhci.cap_regs->hcsparams1 >> 24) & 0xFF;

    // 7. Allocate Device Context Base Address Array (DCBAA)
    void *dcbaa_phys = pmm_alloc();
    g_xhci.dcbaa_phys = (uint64_t)dcbaa_phys;
    g_xhci.dcbaa_virt = (uint64_t *)VIRT(dcbaa_phys);
    memset(g_xhci.dcbaa_virt, 0, PAGE_SIZE);

    g_xhci.op_regs->config = g_xhci.max_slots;
    g_xhci.op_regs->dcbaap = g_xhci.dcbaa_phys;

    // 8. Allocate Command Ring
    void *cmd_phys = pmm_alloc();
    g_xhci.cmd_ring_phys = (uint64_t)cmd_phys;
    g_xhci.cmd_ring_virt = (xhci_trb_t *)VIRT(cmd_phys);
    memset(g_xhci.cmd_ring_virt, 0, PAGE_SIZE);
    g_xhci.cmd_enqueue_idx = 0;
    g_xhci.cmd_pcs = 1; // Producer Cycle State starts at 1

    g_xhci.op_regs->crcr = g_xhci.cmd_ring_phys | 1U; // Enable RCS (Ring Cycle State)

    // 9. Allocate Event Ring & Event Ring Segment Table (ERST)
    void *event_phys = pmm_alloc();
    g_xhci.event_ring_phys = (uint64_t)event_phys;
    g_xhci.event_ring_virt = (xhci_trb_t *)VIRT(event_phys);
    memset(g_xhci.event_ring_virt, 0, PAGE_SIZE);
    g_xhci.event_dequeue_idx = 0;
    g_xhci.event_ccs = 1;

    void *erst_phys = pmm_alloc();
    g_xhci.erst_phys = (uint64_t)erst_phys;
    g_xhci.erst_virt = (xhci_erst_entry_t *)VIRT(erst_phys);
    memset(g_xhci.erst_virt, 0, PAGE_SIZE);

    g_xhci.erst_virt[0].ring_segment_base = g_xhci.event_ring_phys;
    g_xhci.erst_virt[0].ring_segment_size = XHCI_TRB_RING_SIZE;

    g_xhci.rt_regs->ir[0].erstsz = 1;
    g_xhci.rt_regs->ir[0].erdp   = g_xhci.event_ring_phys | 0x8U; // Clear EHB bit
    g_xhci.rt_regs->ir[0].erstba = g_xhci.erst_phys;
    g_xhci.rt_regs->ir[0].iman  |= 2U; // Enable Interrupts

    // 10. Start Host Controller (RS = 1)
    g_xhci.op_regs->usbcmd |= 1U;
    while (g_xhci.op_regs->usbsts & 1U) {
        // Wait until HCHalted clears
    }

    serial_puts(COM1, "[xHCI] USB 3.0 Host Controller successfully started!\n");
    return 0;
}

static pci_device_id_t xhci_pci_ids[] = {
    { 0xFFFF, 0xFFFF, 0x0C, 0x03 }, // Serial Bus Controller (0x0C), USB Subclass (0x03)
    { 0, 0, 0, 0 }
};

static pci_driver_t xhci_pci_driver = {
    .name     = "xHCI USB 3.0 Host Controller",
    .id_table = xhci_pci_ids,
    .probe    = xhci_probe,
    .remove   = NULL,
    .next     = NULL
};

static int __init xhci_module_init(void) {
    pci_register_driver(&xhci_pci_driver);
    return 0;
}

// Регистрируем через выделенный usb_initcall
usb_initcall(xhci_module_init);