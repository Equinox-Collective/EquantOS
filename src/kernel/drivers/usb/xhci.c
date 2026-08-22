// src/kernel/drivers/usb/xhci.c - Detailed Logs, Doorbells & USB HID Keyboard Polling
#include "xhci.h"
#include "../pci/pci.h"
#include "../../core/mem/pmm.h"
#include "../../core/mem/vmm.h"
#include "../serial/serial.h"
#include "../input.h"
#include "stdio.h"
#include "../../core/initcall.h"
#include "string.h"

static xhci_t g_xhci;

// Standard USB HID Boot Protocol Scancode to EquantOS Input Code Map
static const uint16_t hid_scancode_map[256] = {
    [0x04] = KEY_A, [0x05] = KEY_B, [0x06] = KEY_C, [0x07] = KEY_D,
    [0x08] = KEY_E, [0x09] = KEY_F, [0x0A] = KEY_G, [0x0B] = KEY_H,
    [0x0C] = KEY_I, [0x0D] = KEY_J, [0x0E] = KEY_K, [0x0F] = KEY_L,
    [0x10] = KEY_M, [0x11] = KEY_N, [0x12] = KEY_O, [0x13] = KEY_P,
    [0x14] = KEY_Q, [0x15] = KEY_R, [0x16] = KEY_S, [0x17] = KEY_T,
    [0x18] = KEY_U, [0x19] = KEY_V, [0x1A] = KEY_W, [0x1B] = KEY_X,
    [0x1C] = KEY_Y, [0x1D] = KEY_Z,
    [0x1E] = KEY_1, [0x1F] = KEY_2, [0x20] = KEY_3, [0x21] = KEY_4,
    [0x22] = KEY_5, [0x23] = KEY_6, [0x24] = KEY_7, [0x25] = KEY_8,
    [0x26] = KEY_9, [0x27] = KEY_0,
    [0x28] = KEY_ENTER,     [0x29] = KEY_ESC,       [0x2A] = KEY_BACKSPACE,
    [0x2B] = KEY_TAB,       [0x2C] = KEY_SPACE,     [0x2D] = KEY_MINUS,
    [0x2E] = KEY_EQUAL,     [0x4F] = KEY_RIGHT,     [0x50] = KEY_LEFT,
    [0x51] = KEY_DOWN,      [0x52] = KEY_UP
};

void xhci_ring_doorbell(uint32_t slot, uint32_t target) {
    if (!g_xhci.db_regs) return;
    
    char buf[64];
    snprintf(buf, sizeof(buf), "[xHCI-DOORBELL] Ringing Doorbell Register for Slot %u (Target: %u)\n", 
             (unsigned int)slot, (unsigned int)target);
    serial_puts(COM1, buf);

    g_xhci.db_regs[slot] = target;
}

static void xhci_post_command(uint64_t param, uint32_t status, uint32_t control) {
    xhci_trb_t *trb = &g_xhci.cmd_ring_virt[g_xhci.cmd_enqueue_idx];
    trb->parameter = param;
    trb->status    = status;
    trb->control   = (control & ~1U) | g_xhci.cmd_pcs;

    char buf[128];
    snprintf(buf, sizeof(buf), "[xHCI-CMD] Enqueued TRB #%u | Type: %u | Param: 0x%lx\n",
             (unsigned int)g_xhci.cmd_enqueue_idx, (unsigned int)((control >> 10) & 0x3F), param);
    serial_puts(COM1, buf);

    g_xhci.cmd_enqueue_idx++;
    if (g_xhci.cmd_enqueue_idx >= XHCI_TRB_RING_SIZE - 1) {
        xhci_trb_t *link = &g_xhci.cmd_ring_virt[g_xhci.cmd_enqueue_idx];
        link->parameter = g_xhci.cmd_ring_phys;
        link->status    = 0;
        link->control   = (TRB_TYPE_LINK << 10) | (1U << 1) | g_xhci.cmd_pcs;
        
        g_xhci.cmd_enqueue_idx = 0;
        g_xhci.cmd_pcs ^= 1;
    }

    // Ring Doorbell 0 for Host Controller Command Ring execution
    xhci_ring_doorbell(0, 0);
}

void xhci_poll_event_ring(void) {
    if (!g_xhci.event_ring_virt) return;

    xhci_trb_t *event = &g_xhci.event_ring_virt[g_xhci.event_dequeue_idx];
    uint8_t cycle = event->control & 1U;

    // Check if Event TRB Cycle bit matches Consumer Cycle State (CCS)
    if (cycle == g_xhci.event_ccs) {
        uint8_t trb_type = (event->control >> 10) & 0x3F;
        uint8_t completion_code = (event->status >> 24) & 0xFF;

        char buf[128];
        snprintf(buf, sizeof(buf), "[xHCI-EVENT] Recv TRB #%u | Type: %u | Code: %u | Param: 0x%lx\n",
                 (unsigned int)g_xhci.event_dequeue_idx, (unsigned int)trb_type, 
                 (unsigned int)completion_code, event->parameter);
        serial_puts(COM1, buf);

        // Advance Event Ring Dequeue Index
        g_xhci.event_dequeue_idx++;
        if (g_xhci.event_dequeue_idx >= XHCI_TRB_RING_SIZE) {
            g_xhci.event_dequeue_idx = 0;
            g_xhci.event_ccs ^= 1;
        }

        // Update Event Ring Dequeue Pointer Register (ERDP)
        uint64_t erdp_val = g_xhci.event_ring_phys + (g_xhci.event_dequeue_idx * sizeof(xhci_trb_t));
        g_xhci.rt_regs->ir[0].erdp = erdp_val | 0x8U; // Clear Event Handler Busy (EHB)
    }
}

void xhci_scan_ports(void) {
    if (!g_xhci.port_regs) return;

    serial_puts(COM1, "[xHCI] Scanning xHCI Root Hub Ports...\n");

    for (uint32_t i = 0; i < g_xhci.max_ports; i++) {
        uint32_t portsc = g_xhci.port_regs[i].portsc;

        if (portsc & PORTSC_CCS) { // Current Connect Status == 1
            uint32_t speed = (portsc >> 10) & 0x0F;
            char buf[128];
            snprintf(buf, sizeof(buf), "[xHCI-PORT] Device Detected on Port %u! Speed ID: %u | PORTSC: 0x%08x\n",
                     (unsigned int)(i + 1), (unsigned int)speed, (unsigned int)portsc);
            serial_puts(COM1, buf);

            // Issue Port Reset (PR = 1)
            serial_puts(COM1, "[xHCI-PORT] Issuing Port Reset...\n");
            g_xhci.port_regs[i].portsc = (portsc & ~PORTSC_PED) | PORTSC_PR;

            // Wait for Port Reset Completion
            while (g_xhci.port_regs[i].portsc & PORTSC_PR) {
                // Busy wait for PR clearing
            }

            serial_puts(COM1, "[xHCI-PORT] Port Reset Complete! Port Enabled.\n");
        }
    }
}

static int xhci_probe(pci_device_t *dev) {
    serial_puts(COM1, "============================================================\n");
    serial_puts(COM1, "[xHCI] USB 3.0 Host Controller PCI Driver Probed!\n");

    // Enable MMIO Space & Bus Master in PCI Command Register
    pci_write_word(dev->bus, dev->slot, dev->func, 0x04, 0x0006);

    uint32_t bar0_low  = pci_read_dword(dev->bus, dev->slot, dev->func, 0x10);
    uint32_t bar0_high = pci_read_dword(dev->bus, dev->slot, dev->func, 0x14);
    uint64_t bar0 = ((uint64_t)bar0_high << 32) | (bar0_low & ~0x0FULL);

    char logbuf[128];
    snprintf(logbuf, sizeof(logbuf), "[xHCI-MMIO] Physical BAR0 Address: 0x%lx\n", bar0);
    serial_puts(COM1, logbuf);

    if (!bar0) {
        serial_puts(COM1, "[xHCI-ERROR] Invalid BAR0 Base Address!\n");
        return -1;
    }

    // Map xHCI MMIO Space
    g_xhci.cap_regs  = (volatile xhci_cap_regs_t *)pci_map_mmio(bar0, 0x10000);
    g_xhci.op_regs   = (volatile xhci_op_regs_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->caplength);
    g_xhci.rt_regs   = (volatile xhci_rt_regs_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->rtoff);
    g_xhci.db_regs   = (volatile uint32_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->dboff);
    g_xhci.port_regs = (volatile xhci_port_regs_t *)((uintptr_t)g_xhci.op_regs + 0x400);

    snprintf(logbuf, sizeof(logbuf), "[xHCI-CAP] CapLength: %u | HCVersion: 0x%04x | Doorbell Offset: 0x%x\n",
             g_xhci.cap_regs->caplength, g_xhci.cap_regs->hciversion, g_xhci.cap_regs->dboff);
    serial_puts(COM1, logbuf);

    // Stop Controller before Reset
    g_xhci.op_regs->usbcmd &= ~1U;
    while (!(g_xhci.op_regs->usbsts & 1U)) {}

    // Reset Controller
    g_xhci.op_regs->usbcmd |= (1U << 1);
    while (g_xhci.op_regs->usbcmd & (1U << 1)) {}
    while (g_xhci.op_regs->usbsts & (1U << 11)) {}

    g_xhci.max_slots = g_xhci.cap_regs->hcsparams1 & 0xFF;
    g_xhci.max_ports = (g_xhci.cap_regs->hcsparams1 >> 24) & 0xFF;

    snprintf(logbuf, sizeof(logbuf), "[xHCI-CAP] Max Device Slots: %u | Max Ports: %u\n",
             (unsigned int)g_xhci.max_slots, (unsigned int)g_xhci.max_ports);
    serial_puts(COM1, logbuf);

    // Allocate Device Context Base Address Array (DCBAA)
    void *dcbaa_phys = pmm_alloc();
    g_xhci.dcbaa_phys = (uint64_t)dcbaa_phys;
    g_xhci.dcbaa_virt = (uint64_t *)VIRT(dcbaa_phys);
    memset(g_xhci.dcbaa_virt, 0, PAGE_SIZE);

    snprintf(logbuf, sizeof(logbuf), "[xHCI-DMA] DCBAA Allocated at Physical: 0x%lx\n", g_xhci.dcbaa_phys);
    serial_puts(COM1, logbuf);

    g_xhci.op_regs->config = g_xhci.max_slots;
    g_xhci.op_regs->dcbaap = g_xhci.dcbaa_phys;

    // Allocate Command Ring
    void *cmd_phys = pmm_alloc();
    g_xhci.cmd_ring_phys = (uint64_t)cmd_phys;
    g_xhci.cmd_ring_virt = (xhci_trb_t *)VIRT(cmd_phys);
    memset(g_xhci.cmd_ring_virt, 0, PAGE_SIZE);
    g_xhci.cmd_enqueue_idx = 0;
    g_xhci.cmd_pcs = 1;

    snprintf(logbuf, sizeof(logbuf), "[xHCI-DMA] Command Ring Allocated at Physical: 0x%lx\n", g_xhci.cmd_ring_phys);
    serial_puts(COM1, logbuf);

    g_xhci.op_regs->crcr = g_xhci.cmd_ring_phys | 1U;

    // Allocate Event Ring & ERST Table
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
    g_xhci.rt_regs->ir[0].erdp   = g_xhci.event_ring_phys | 0x8U;
    g_xhci.rt_regs->ir[0].erstba = g_xhci.erst_phys;
    g_xhci.rt_regs->ir[0].iman  |= 2U;

    snprintf(logbuf, sizeof(logbuf), "[xHCI-DMA] Event Ring Allocated at Physical: 0x%lx\n", g_xhci.event_ring_phys);
    serial_puts(COM1, logbuf);

    // Start Controller (RS = 1)
    g_xhci.op_regs->usbcmd |= 1U;
    while (g_xhci.op_regs->usbsts & 1U) {}

    serial_puts(COM1, "[xHCI-STATUS] Controller State: RUNNING (USBCMD RS=1)\n");

    // Test Doorbell Ring
    xhci_post_command(0, 0, (TRB_TYPE_ENABLE_SLOT << 10));

    // Poll Event Ring for Command Completion TRB
    xhci_poll_event_ring();

    // Scan Root Hub Ports for connected devices
    xhci_scan_ports();

    serial_puts(COM1, "============================================================\n");
    return 0;
}

static pci_device_id_t xhci_pci_ids[] = {
    { 0xFFFF, 0xFFFF, 0x0C, 0x03 },
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