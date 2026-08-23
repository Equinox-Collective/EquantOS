// src/kernel/drivers/usb/xhci.c - Safe IRQ-Context xHCI Driver (Zero Allocations in ISR)
#include "xhci.h"
#include "usb_hid.h"
#include "../pci/pci.h"
#include "../../core/mem/pmm.h"
#include "../../core/mem/vmm.h"
#include "../serial/serial.h"
#include "stdio.h"
#include "../../core/initcall.h"
#include "string.h"

// xHCI Slot Command States
typedef enum {
    XHCI_SLOT_STATE_DISABLED   = 0,
    XHCI_SLOT_STATE_ENABLING   = 1,
    XHCI_SLOT_STATE_ADDRESSING = 2,
    XHCI_SLOT_STATE_ADDRESSED  = 3
} xhci_slot_state_t;

static xhci_t g_xhci;
static uint32_t active_slot_id = 1;
static xhci_slot_state_t slot_cmd_state = XHCI_SLOT_STATE_DISABLED;

// Input Control Context Structures
typedef struct {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved[6];
} __attribute__((packed)) xhci_input_ctrl_ctx_t;

typedef struct {
    xhci_input_ctrl_ctx_t ctrl;
    xhci_slot_ctx_t       slot;
    xhci_ep_ctx_t         ep0;
    xhci_ep_ctx_t         ep1_out;
    xhci_ep_ctx_t         ep1_in;
} __attribute__((packed)) xhci_input_ctx_t;

// Pre-allocated Contexts & Buffers
static xhci_input_ctx_t *input_ctx_virt = NULL;
static uint64_t          input_ctx_phys = 0;
static void             *output_ctx_virt = NULL;
static uint64_t          output_ctx_phys = 0;

static xhci_trb_t *ep1_transfer_ring_virt = NULL;
static uint64_t    ep1_transfer_ring_phys = 0;
static uint32_t    ep1_enqueue_idx = 0;
static uint8_t     ep1_pcs = 1;

static uint8_t *hid_report_buffer_virt = NULL;
static uint64_t hid_report_buffer_phys = 0;

void xhci_ring_doorbell(uint32_t slot, uint32_t target) {
    if (!g_xhci.db_regs) return;
    
    char buf[128];
    snprintf(buf, sizeof(buf), "[xHCI-DOORBELL] Ringing Doorbell for Slot %u (Target Endpoint: %u)\n", 
             (unsigned int)slot, (unsigned int)target);
    serial_puts(COM1, buf);

    g_xhci.db_regs[slot] = target;
}

static void xhci_post_command(uint64_t param, uint32_t status, uint32_t control) {
    xhci_trb_t *trb = &g_xhci.cmd_ring_virt[g_xhci.cmd_enqueue_idx];
    trb->parameter = param;
    trb->status    = status;
    trb->control   = (control & ~1U) | g_xhci.cmd_pcs;

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

// Arm Endpoint 1 IN Transfer Ring and Ring Doorbell Target 3
static void xhci_arm_keyboard_endpoint(uint32_t slot_id) {
    if (!ep1_transfer_ring_virt || !hid_report_buffer_virt) return;

    xhci_trb_t *trb = &ep1_transfer_ring_virt[ep1_enqueue_idx];
    trb->parameter = hid_report_buffer_phys;
    trb->status    = 8; // Expect 8 bytes HID report
    trb->control   = (TRB_TYPE_NORMAL << 10) | (1U << 5) | ep1_pcs; // IOC=1

    ep1_enqueue_idx++;
    if (ep1_enqueue_idx >= 15) {
        xhci_trb_t *link = &ep1_transfer_ring_virt[ep1_enqueue_idx];
        link->parameter = ep1_transfer_ring_phys;
        link->status    = 0;
        link->control   = (TRB_TYPE_LINK << 10) | (1U << 1) | ep1_pcs;
        
        ep1_enqueue_idx = 0;
        ep1_pcs ^= 1;
    }

    // Ring Doorbell for Slot ID, Target 3 (Endpoint 1 IN)
    xhci_ring_doorbell(slot_id, 3);
}

// Configure Slot & Issue Address Device Command
static void xhci_address_device(uint32_t slot_id) {
    if (!input_ctx_virt) return;

    memset(input_ctx_virt, 0, sizeof(xhci_input_ctx_t));
    memset(output_ctx_virt, 0, PAGE_SIZE);

    // Bind Output Context to DCBAA Slot
    g_xhci.dcbaa_virt[slot_id] = output_ctx_phys;

    // Enable Slot Context, EP0 Context, and EP1 IN Context
    input_ctx_virt->ctrl.add_flags = (1U << 0) | (1U << 1) | (1U << 3);

    // Slot Context Setup
    input_ctx_virt->slot.info1 = (3U << 27) | (1U << 16); // Context Entries = 3, Root Hub Port = 1
    input_ctx_virt->slot.info2 = (3U << 20); // Speed = High/Full

    // EP1 IN Context Setup (Keyboard Interrupt IN)
    input_ctx_virt->ep1_in.info1 = (7U << 3); // EP Type = Interrupt IN
    input_ctx_virt->ep1_in.info2 = (8U << 16) | (3U << 1); // Max Packet Size = 8, Error Count = 3
    input_ctx_virt->ep1_in.tr_dequeue_ptr = ep1_transfer_ring_phys | 1U;
    input_ctx_virt->ep1_in.avg_trb_len = 8;

    serial_puts(COM1, "[xHCI-ADDRESS] Issuing Address Device Command TRB...\n");
    xhci_post_command(input_ctx_phys, 0, (TRB_TYPE_ADDRESS_DEVICE << 10) | (slot_id << 24));
}

void xhci_handle_events(void) {
    if (!g_xhci.event_ring_virt) return;

    while (1) {
        xhci_trb_t *event = &g_xhci.event_ring_virt[g_xhci.event_dequeue_idx];
        uint8_t cycle = event->control & 1U;

        if (cycle != g_xhci.event_ccs) break;

        uint8_t trb_type = (event->control >> 10) & 0x3F;
        uint8_t completion_code = (event->status >> 24) & 0xFF;
        uint32_t slot_id = (event->control >> 24) & 0xFF;

        char log_buf[128];
        snprintf(log_buf, sizeof(log_buf), "[xHCI-EVENT] TRB #%u | Type: %u | Code: %u | Slot: %u\n",
                 (unsigned int)g_xhci.event_dequeue_idx, (unsigned int)trb_type, 
                 (unsigned int)completion_code, (unsigned int)slot_id);
        serial_puts(COM1, log_buf);

        // 1. Command Completion State Machine
        if (trb_type == TRB_TYPE_CMD_COMPLETION) {
            if (slot_cmd_state == XHCI_SLOT_STATE_ENABLING && completion_code == 1) {
                slot_cmd_state = XHCI_SLOT_STATE_ADDRESSING;
                active_slot_id = slot_id;
                serial_puts(COM1, "[xHCI-SLOT] Slot Enabled! Addressing USB Device...\n");
                xhci_address_device(active_slot_id);
            } 
            else if (slot_cmd_state == XHCI_SLOT_STATE_ADDRESSING && completion_code == 1) {
                slot_cmd_state = XHCI_SLOT_STATE_ADDRESSED;
                serial_puts(COM1, "[xHCI-SLOT] Address Device Complete! Arming EP1 IN...\n");
                xhci_arm_keyboard_endpoint(active_slot_id);
            }
        } 
        // 2. Transfer Event (USB HID Report Received)
        else if (trb_type == TRB_TYPE_TRANSFER_EVENT) {
            serial_puts(COM1, "[xHCI-HID] Transfer Event Received! Passing HID Report to Layer 3 Driver...\n");
            
            // Pass raw 8-byte HID report buffer to Layer 3 USB HID Driver
            usb_hid_parse_keyboard_report(hid_report_buffer_virt, 8);

            // Re-arm Transfer Ring for next USB keystroke
            xhci_arm_keyboard_endpoint(active_slot_id);
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

void xhci_timer_tick(void) {
    xhci_handle_events();
}

void xhci_scan_ports(void) {
    if (!g_xhci.port_regs) return;

    for (uint32_t i = 0; i < g_xhci.max_ports; i++) {
        uint32_t portsc = g_xhci.port_regs[i].portsc;

        if (portsc & PORTSC_CCS) {
            serial_puts(COM1, "[xHCI-PORT] Resetting Connected USB Device Port...\n");
            g_xhci.port_regs[i].portsc = (portsc & ~PORTSC_PED) | PORTSC_PR;
            while (g_xhci.port_regs[i].portsc & PORTSC_PR) {}
        }
    }
}

static int xhci_probe(pci_device_t *dev) {
    serial_puts(COM1, "[xHCI] Registering Layer 2 USB Host Controller...\n");

    pci_write_word(dev->bus, dev->slot, dev->func, 0x04, 0x0006);

    uint32_t bar0_low  = pci_read_dword(dev->bus, dev->slot, dev->func, 0x10);
    uint32_t bar0_high = pci_read_dword(dev->bus, dev->slot, dev->func, 0x14);
    uint64_t bar0 = ((uint64_t)bar0_high << 32) | (bar0_low & ~0x0FULL);

    if (!bar0) return -1;

    g_xhci.cap_regs  = (volatile xhci_cap_regs_t *)pci_map_mmio(bar0, 0x10000);
    g_xhci.op_regs   = (volatile xhci_op_regs_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->caplength);
    g_xhci.rt_regs   = (volatile xhci_rt_regs_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->rtoff);
    g_xhci.db_regs   = (volatile uint32_t *)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->dboff);
    g_xhci.port_regs = (volatile xhci_port_regs_t *)((uintptr_t)g_xhci.op_regs + 0x400);

    g_xhci.op_regs->usbcmd &= ~1U;
    while (!(g_xhci.op_regs->usbsts & 1U)) {}

    g_xhci.op_regs->usbcmd |= (1U << 1);
    while (g_xhci.op_regs->usbcmd & (1U << 1)) {}
    while (g_xhci.op_regs->usbsts & (1U << 11)) {}

    g_xhci.max_slots = g_xhci.cap_regs->hcsparams1 & 0xFF;
    g_xhci.max_ports = (g_xhci.cap_regs->hcsparams1 >> 24) & 0xFF;

    // Pre-allocate DCBAA
    void *dcbaa_phys = pmm_alloc();
    g_xhci.dcbaa_phys = (uint64_t)dcbaa_phys;
    g_xhci.dcbaa_virt = (uint64_t *)VIRT(dcbaa_phys);
    memset(g_xhci.dcbaa_virt, 0, PAGE_SIZE);

    g_xhci.op_regs->config = g_xhci.max_slots;
    g_xhci.op_regs->dcbaap = g_xhci.dcbaa_phys;

    // Pre-allocate Command Ring
    void *cmd_phys = pmm_alloc();
    g_xhci.cmd_ring_phys = (uint64_t)cmd_phys;
    g_xhci.cmd_ring_virt = (xhci_trb_t *)VIRT(cmd_phys);
    memset(g_xhci.cmd_ring_virt, 0, PAGE_SIZE);
    g_xhci.cmd_enqueue_idx = 0;
    g_xhci.cmd_pcs = 1;

    g_xhci.op_regs->crcr = g_xhci.cmd_ring_phys | 1U;

    // Pre-allocate Event Ring
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

    // Pre-allocate Input & Output Contexts
    void *in_phys = pmm_alloc();
    input_ctx_phys = (uint64_t)in_phys;
    input_ctx_virt = (xhci_input_ctx_t *)VIRT(in_phys);

    void *out_phys = pmm_alloc();
    output_ctx_phys = (uint64_t)out_phys;
    output_ctx_virt = (void *)VIRT(out_phys);

    // Pre-allocate EP1 Transfer Ring & DMA HID Buffer
    void *tr_phys = pmm_alloc();
    ep1_transfer_ring_phys = (uint64_t)tr_phys;
    ep1_transfer_ring_virt = (xhci_trb_t *)VIRT(tr_phys);
    memset(ep1_transfer_ring_virt, 0, PAGE_SIZE);

    void *buf_phys = pmm_alloc();
    hid_report_buffer_phys = (uint64_t)buf_phys;
    hid_report_buffer_virt = (uint8_t *)VIRT(buf_phys);
    memset(hid_report_buffer_virt, 0, PAGE_SIZE);

    // Start Controller
    g_xhci.op_regs->usbcmd |= 1U;
    while (g_xhci.op_regs->usbsts & 1U) {}

    // Set State Machine to ENABLING
    slot_cmd_state = XHCI_SLOT_STATE_ENABLING;

    xhci_scan_ports();
    xhci_post_command(0, 0, (TRB_TYPE_ENABLE_SLOT << 10));

    serial_puts(COM1, "[xHCI] Layer 2 Controller Active & State Machine Armed.\n");
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