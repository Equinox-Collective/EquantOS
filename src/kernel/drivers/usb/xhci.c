// src/kernel/drivers/usb/xhci.c - Production xHCI Slot & Event Engine
#include "xhci.h"
#include "../pci/pci.h"
#include "../../core/mem/pmm.h"
#include "../../core/mem/vmm.h"
#include "../../core/initcall.h"
#include "../serial/serial.h"
#include "../input.h"
#include "stdio.h"
#include "string.h"

static xhci_t g_xhci;
static uint32_t active_slot_id = 0;
static uint32_t active_port_num = 0;

void xhci_ring_doorbell(uint32_t slot, uint32_t target) {
    if (!g_xhci.db_regs) return;
    
    char buf[128];
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

    xhci_ring_doorbell(0, 0);
}

void xhci_poll_event_ring(void) {
    if (!g_xhci.event_ring_virt) return;

    // Drain all pending TRBs from Event Ring
    while (1) {
        xhci_trb_t *event = &g_xhci.event_ring_virt[g_xhci.event_dequeue_idx];
        uint8_t cycle = event->control & 1U;

        // Break if cycle bit doesn't match Consumer Cycle State (no new events)
        if (cycle != g_xhci.event_ccs) {
            break;
        }

        uint8_t trb_type = (event->control >> 10) & 0x3F;
        uint8_t completion_code = (event->status >> 24) & 0xFF;
        uint32_t slot_id = (event->control >> 24) & 0xFF;

        char buf[128];
        snprintf(buf, sizeof(buf), "[xHCI-EVENT] Recv TRB #%u | Type: %u | Code: %u | SlotID: %u | Param: 0x%lx\n",
                 (unsigned int)g_xhci.event_dequeue_idx, (unsigned int)trb_type, 
                 (unsigned int)completion_code, (unsigned int)slot_id, event->parameter);
        serial_puts(COM1, buf);

        if (trb_type == TRB_TYPE_CMD_COMPLETION) {
            if (completion_code == 1) { // Success
                if (slot_id > 0) {
                    active_slot_id = slot_id;
                    snprintf(buf, sizeof(buf), "[xHCI-SLOT] Slot ID %u Successfully Enabled by xHCI!\n", active_slot_id);
                    serial_puts(COM1, buf);
                }
            }
        } else if (trb_type == TRB_TYPE_PORT_STATUS_CHG) {
            serial_puts(COM1, "[xHCI-EVENT] Port Status Change Event Handled.\n");
        }

        g_xhci.event_dequeue_idx++;
        if (g_xhci.event_dequeue_idx >= XHCI_TRB_RING_SIZE) {
            g_xhci.event_dequeue_idx = 0;
            g_xhci.event_ccs ^= 1;
        }

        uint64_t erdp_val = g_xhci.event_ring_phys + (g_xhci.event_dequeue_idx * sizeof(xhci_trb_t));
        g_xhci.rt_regs->ir[0].erdp = erdp_val | 0x8U;
    }
}

void xhci_scan_ports(void) {
    if (!g_xhci.port_regs) return;

    serial_puts(COM1, "[xHCI] Scanning xHCI Root Hub Ports...\n");

    for (uint32_t i = 0; i < g_xhci.max_ports; i++) {
        uint32_t portsc = g_xhci.port_regs[i].portsc;

        if (portsc & PORTSC_CCS) { // Device Connected
            active_port_num = i + 1;
            uint32_t speed = (portsc >> 10) & 0x0F;
            char buf[128];
            snprintf(buf, sizeof(buf), "[xHCI-PORT] USB Device Detected on Port %u! Speed ID: %u | PORTSC: 0x%08x\n",
                     (unsigned int)active_port_num, (unsigned int)speed, (unsigned int)portsc);
            serial_puts(COM1, buf);

            // Reset Port
            serial_puts(COM1, "[xHCI-PORT] Issuing Port Reset...\n");
            g_xhci.port_regs[i].portsc = (portsc & ~PORTSC_PED) | PORTSC_PR;

            while (g_xhci.port_regs[i].portsc & PORTSC_PR) {}

            serial_puts(COM1, "[xHCI-PORT] Port Reset Complete! Port Enabled.\n");
        }
    }
}

static int xhci_probe(pci_device_t *dev) {
    serial_puts(COM1, "============================================================\n");
    serial_puts(COM1, "[xHCI] USB 3.0 Host Controller PCI Driver Probed!\n");

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

    g_xhci.cap_regs  = (volatile xhci_cap_regs_t *)pci_map_mmio(bar0, 0x10000);
    g_xhci.op_regs   = (volatile xhci_op_regs_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->caplength);
    g_xhci.rt_regs   = (volatile xhci_rt_regs_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->rtoff);
    g_xhci.db_regs   = (volatile uint32_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->dboff);
    g_xhci.port_regs = (volatile xhci_port_regs_t *)((uintptr_t)g_xhci.op_regs + 0x400);

    snprintf(logbuf, sizeof(logbuf), "[xHCI-CAP] CapLength: %u | HCVersion: 0x%04x | Doorbell Offset: 0x%x\n",
             g_xhci.cap_regs->caplength, g_xhci.cap_regs->hciversion, g_xhci.cap_regs->dboff);
    serial_puts(COM1, logbuf);

    g_xhci.op_regs->usbcmd &= ~1U;
    while (!(g_xhci.op_regs->usbsts & 1U)) {}

    g_xhci.op_regs->usbcmd |= (1U << 1);
    while (g_xhci.op_regs->usbcmd & (1U << 1)) {}
    while (g_xhci.op_regs->usbsts & (1U << 11)) {}

    g_xhci.max_slots = g_xhci.cap_regs->hcsparams1 & 0xFF;
    g_xhci.max_ports = (g_xhci.cap_regs->hcsparams1 >> 24) & 0xFF;

    snprintf(logbuf, sizeof(logbuf), "[xHCI-CAP] Max Device Slots: %u | Max Ports: %u\n",
             (unsigned int)g_xhci.max_slots, (unsigned int)g_xhci.max_ports);
    serial_puts(COM1, logbuf);

    void *dcbaa_phys = pmm_alloc();
    g_xhci.dcbaa_phys = (uint64_t)dcbaa_phys;
    g_xhci.dcbaa_virt = (uint64_t *)VIRT(dcbaa_phys);
    memset(g_xhci.dcbaa_virt, 0, PAGE_SIZE);

    snprintf(logbuf, sizeof(logbuf), "[xHCI-DMA] DCBAA Allocated at Physical: 0x%lx\n", g_xhci.dcbaa_phys);
    serial_puts(COM1, logbuf);

    g_xhci.op_regs->config = g_xhci.max_slots;
    g_xhci.op_regs->dcbaap = g_xhci.dcbaa_phys;

    void *cmd_phys = pmm_alloc();
    g_xhci.cmd_ring_phys = (uint64_t)cmd_phys;
    g_xhci.cmd_ring_virt = (xhci_trb_t *)VIRT(cmd_phys);
    memset(g_xhci.cmd_ring_virt, 0, PAGE_SIZE);
    g_xhci.cmd_enqueue_idx = 0;
    g_xhci.cmd_pcs = 1;

    snprintf(logbuf, sizeof(logbuf), "[xHCI-DMA] Command Ring Allocated at Physical: 0x%lx\n", g_xhci.cmd_ring_phys);
    serial_puts(COM1, logbuf);

    g_xhci.op_regs->crcr = g_xhci.cmd_ring_phys | 1U;

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

    g_xhci.op_regs->usbcmd |= 1U;
    while (g_xhci.op_regs->usbsts & 1U) {}

    serial_puts(COM1, "[xHCI-STATUS] Controller State: RUNNING (USBCMD RS=1)\n");

    // Scan Root Hub Ports for connected devices
    xhci_scan_ports();

    // Enable Slot for Device
    xhci_post_command(0, 0, (TRB_TYPE_ENABLE_SLOT << 10));

    // Drain initial events
    xhci_poll_event_ring();

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

static int xhci_subsys_init(void) {
    pci_register_driver(&xhci_pci_driver);
    return 0;
}

usb_initcall(xhci_subsys_init);