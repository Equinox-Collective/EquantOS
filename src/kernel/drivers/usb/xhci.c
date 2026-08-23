// src/kernel/drivers/usb/xhci.c - xHCI Engine with Exact 0x08 TR Dequeue Pointer Offset
#include "xhci.h"
#include "usb_hid.h"
#include "../pci/pci.h"
#include "../serial/serial.h"
#include "../../core/initcall.h"
#include "../../core/mem/pmm.h"
#include "../../core/mem/vmm.h"
#include "stdio.h"
#include "string.h"

typedef enum {
    XHCI_SLOT_STATE_DISABLED      = 0,
    XHCI_SLOT_STATE_ENABLING      = 1,
    XHCI_SLOT_STATE_ADDRESSING    = 2,
    XHCI_SLOT_STATE_CONFIGURING   = 3,
    XHCI_SLOT_STATE_SETTING_CONFIG= 4,
    XHCI_SLOT_STATE_ADDRESSED     = 5
} xhci_slot_state_t;

static xhci_t g_xhci;
static uint32_t active_slot_id = 1;
static uint32_t active_port_num = 1;
static uint32_t active_port_speed = 3;
static xhci_slot_state_t slot_cmd_state = XHCI_SLOT_STATE_DISABLED;

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

static xhci_input_ctx_t *input_ctx_virt = NULL;
static uint64_t          input_ctx_phys = 0;
static void             *output_ctx_virt = NULL;
static uint64_t          output_ctx_phys = 0;

static xhci_trb_t *ep0_transfer_ring_virt = NULL;
static uint64_t    ep0_transfer_ring_phys = 0;

static xhci_trb_t *ep1_transfer_ring_virt = NULL;
static uint64_t    ep1_transfer_ring_phys = 0;
static uint32_t    ep1_enqueue_idx = 0;
static uint8_t     ep1_pcs = 1;

static uint8_t *hid_report_buffer_virt = NULL;
static uint64_t hid_report_buffer_phys = 0;

static const char *xhci_error_string(uint32_t error) {
    switch (error) {
        case 1:  return "Success";
        case 2:  return "Data Buffer Error";
        case 3:  return "Babble Detected Error";
        case 4:  return "USB Transaction Error";
        case 5:  return "TRB Error";
        case 6:  return "Stall Error";
        case 7:  return "Resource Error";
        case 8:  return "Bandwidth Error";
        case 9:  return "No Slots Available Error";
        case 11: return "Slot Not Enabled Error";
        case 12: return "Endpoint Not Enabled Error";
        case 13: return "Short Packet";
        case 17: return "Parameter Error";
        case 19: return "Context State Error";
        default: return "Undefined xHCI Error";
    }
}

void xhci_ring_doorbell(uint32_t slot, uint32_t target) {
    if (!g_xhci.db_regs) return;
    
    char buf[128];
    snprintf(buf, sizeof(buf), "[xHCI-DOORBELL] Ringing Doorbell Slot: %u | Target EP: %u\n", 
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
    snprintf(buf, sizeof(buf), "[xHCI-CMD] Enqueued Cmd TRB #%u | Type: %u | Param: 0x%lx | Ctrl: 0x%x\n",
             (unsigned int)g_xhci.cmd_enqueue_idx, (unsigned int)((control >> 10) & 0x3F), param, (unsigned int)trb->control);
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

static void xhci_arm_keyboard_endpoint(uint32_t slot_id) {
    if (!ep1_transfer_ring_virt || !hid_report_buffer_virt) return;

    xhci_trb_t *trb = &ep1_transfer_ring_virt[ep1_enqueue_idx];
    trb->parameter = hid_report_buffer_phys;
    trb->status    = 8;
    trb->control   = (TRB_TYPE_NORMAL << 10) | (1U << 5) | (1U << 2) | ep1_pcs;

    char buf[128];
    snprintf(buf, sizeof(buf), "[xHCI-ARM] Arming EP1 Ring Index %u | BufferPhys: 0x%lx\n", 
             (unsigned int)ep1_enqueue_idx, hid_report_buffer_phys);
    serial_puts(COM1, buf);

    ep1_enqueue_idx++;
    if (ep1_enqueue_idx >= 15) {
        xhci_trb_t *link = &ep1_transfer_ring_virt[ep1_enqueue_idx];
        link->parameter = ep1_transfer_ring_phys;
        link->status    = 0;
        link->control   = (TRB_TYPE_LINK << 10) | (1U << 1) | ep1_pcs;
        
        ep1_enqueue_idx = 0;
        ep1_pcs ^= 1;
    }

    xhci_ring_doorbell(slot_id, 3);
}

static void xhci_set_configuration(uint32_t slot_id) {
    if (!ep0_transfer_ring_virt) return;

    uint64_t setup_pkt = 0x0000000000010900ULL; // SET_CONFIGURATION(1)

    // 1. Setup Stage TRB
    ep0_transfer_ring_virt[0].parameter = setup_pkt;
    ep0_transfer_ring_virt[0].status    = 8;
    ep0_transfer_ring_virt[0].control   = (TRB_TYPE_SETUP_STAGE << 10) | (1U << 6) | 1U;

    // 2. Status Stage TRB (DIR bit 16 = 1U << 16)
    ep0_transfer_ring_virt[1].parameter = 0;
    ep0_transfer_ring_virt[1].status    = 0;
    ep0_transfer_ring_virt[1].control   = (TRB_TYPE_STATUS_STAGE << 10) | (1U << 5) | (1U << 16) | 1U;

    serial_puts(COM1, "[xHCI-USB] Sending SET_CONFIGURATION(1) Request to EP0...\n");
    xhci_ring_doorbell(slot_id, 1);
}

static void xhci_address_device(uint32_t slot_id) {
    if (!input_ctx_virt) return;

    memset(input_ctx_virt, 0, sizeof(xhci_input_ctx_t));
    memset(output_ctx_virt, 0, PAGE_SIZE);

    g_xhci.dcbaa_virt[slot_id] = output_ctx_phys;

    input_ctx_virt->ctrl.add_flags = 3U; // A0 + A1

    input_ctx_virt->slot.info1 = (1U << 27) | ((active_port_speed & 0x0F) << 20);
    input_ctx_virt->slot.info2 = ((active_port_num & 0xFF) << 16);

    uint32_t max_packet_size = (active_port_speed == 2 || active_port_speed == 1) ? 8 : 64;
    input_ctx_virt->ep0.info0 = 0;
    input_ctx_virt->ep0.info1 = (3U << 1) | (4U << 3) | (max_packet_size << 16);
    input_ctx_virt->ep0.tr_dequeue_ptr = ep0_transfer_ring_phys | 1U;
    input_ctx_virt->ep0.avg_trb_len = 8;

    char log_buf[256];
    snprintf(log_buf, sizeof(log_buf), 
             "[xHCI-ADDRESS] InputCtx AddFlags: 0x%x | Slot Entries: 1 | Port: %u | Speed: %u | EP0_TR_Ptr: 0x%lx\n",
             (unsigned int)input_ctx_virt->ctrl.add_flags, (unsigned int)active_port_num, 
             (unsigned int)active_port_speed, ep0_transfer_ring_phys);
    serial_puts(COM1, log_buf);

    xhci_post_command(input_ctx_phys, 0, (TRB_TYPE_ADDRESS_DEVICE << 10) | (slot_id << 24));
}

static void xhci_configure_keyboard_endpoint(uint32_t slot_id) {
    if (!input_ctx_virt) return;

    memset(input_ctx_virt, 0, sizeof(xhci_input_ctx_t));

    input_ctx_virt->ctrl.add_flags = (1U << 0) | (1U << 3); // A0 + A3

    input_ctx_virt->slot.info1 = (3U << 27) | ((active_port_speed & 0x0F) << 20);
    input_ctx_virt->slot.info2 = ((active_port_num & 0xFF) << 16);

    input_ctx_virt->ep1_in.info0 = (3U << 16); // Interval = 3 (8ms)
    input_ctx_virt->ep1_in.info1 = (3U << 1) | (7U << 3) | (8U << 16); // CErr=3, Interrupt IN, MaxPktSize=8
    input_ctx_virt->ep1_in.tr_dequeue_ptr = ep1_transfer_ring_phys | 1U;
    input_ctx_virt->ep1_in.avg_trb_len = 8;

    char log_buf[256];
    snprintf(log_buf, sizeof(log_buf), 
             "[xHCI-CONFIG] InputCtx AddFlags: 0x%x | Slot Entries: 3 | Port: %u | EP1_TR_Ptr: 0x%lx\n",
             (unsigned int)input_ctx_virt->ctrl.add_flags, (unsigned int)active_port_num, 
             ep1_transfer_ring_phys);
    serial_puts(COM1, log_buf);

    serial_puts(COM1, "[xHCI-CONFIG] Issuing Configure Endpoint Command TRB...\n");
    xhci_post_command(input_ctx_phys, 0, (TRB_TYPE_CONFIG_ENDPOINT << 10) | (slot_id << 24));
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
        snprintf(log_buf, sizeof(log_buf), "[xHCI-EVENT] TRB #%u | Type: %u | Code: %u (%s) | Slot: %u\n",
                 (unsigned int)g_xhci.event_dequeue_idx, (unsigned int)trb_type, 
                 (unsigned int)completion_code, xhci_error_string(completion_code), (unsigned int)slot_id);
        serial_puts(COM1, log_buf);

        if (trb_type == TRB_TYPE_CMD_COMPLETION) {
            if (slot_cmd_state == XHCI_SLOT_STATE_ENABLING && completion_code == 1) {
                slot_cmd_state = XHCI_SLOT_STATE_ADDRESSING;
                active_slot_id = slot_id;
                serial_puts(COM1, "[xHCI-SLOT] Slot Enabled! Addressing USB Device...\n");
                xhci_address_device(active_slot_id);
            } 
            else if (slot_cmd_state == XHCI_SLOT_STATE_ADDRESSING && completion_code == 1) {
                slot_cmd_state = XHCI_SLOT_STATE_CONFIGURING;
                serial_puts(COM1, "[xHCI-SLOT] Address Device Complete! Configuring Keyboard Endpoint...\n");
                xhci_configure_keyboard_endpoint(active_slot_id);
            }
            else if (slot_cmd_state == XHCI_SLOT_STATE_CONFIGURING && completion_code == 1) {
                slot_cmd_state = XHCI_SLOT_STATE_ADDRESSED;
                serial_puts(COM1, "[xHCI-SLOT] Endpoint Configured! Transitioning USB Device to CONFIGURED state...\n");
                
                xhci_set_configuration(active_slot_id);

                serial_puts(COM1, "[xHCI-USB] Arming Keyboard EP1 IN Doorbell (Target 3)...\n");
                xhci_arm_keyboard_endpoint(active_slot_id);
            }
        } 
        else if (trb_type == TRB_TYPE_TRANSFER_EVENT) {
            serial_puts(COM1, "[xHCI-HID] Transfer Event Received! Passing HID Report to Layer 3 Driver...\n");
            
            usb_hid_parse_keyboard_report(hid_report_buffer_virt, 8);

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
            active_port_num = i + 1;
            active_port_speed = (portsc >> 10) & 0x0F;

            char log_buf[128];
            snprintf(log_buf, sizeof(log_buf), "[xHCI-PORT] USB Device Connected on Root Hub Port %u! Speed ID: %u | PORTSC: 0x%08x\n",
                     (unsigned int)active_port_num, (unsigned int)active_port_speed, (unsigned int)portsc);
            serial_puts(COM1, log_buf);

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

    void *dcbaa_phys = pmm_alloc();
    g_xhci.dcbaa_phys = (uint64_t)dcbaa_phys;
    g_xhci.dcbaa_virt = (uint64_t *)VIRT(dcbaa_phys);
    memset(g_xhci.dcbaa_virt, 0, PAGE_SIZE);

    g_xhci.op_regs->config = g_xhci.max_slots;
    g_xhci.op_regs->dcbaap = g_xhci.dcbaa_phys;

    void *cmd_phys = pmm_alloc();
    g_xhci.cmd_ring_phys = (uint64_t)cmd_phys;
    g_xhci.cmd_ring_virt = (xhci_trb_t *)VIRT(cmd_phys);
    memset(g_xhci.cmd_ring_virt, 0, PAGE_SIZE);
    g_xhci.cmd_enqueue_idx = 0;
    g_xhci.cmd_pcs = 1;

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

    void *in_phys = pmm_alloc();
    input_ctx_phys = (uint64_t)in_phys;
    input_ctx_virt = (xhci_input_ctx_t *)VIRT(in_phys);

    void *out_phys = pmm_alloc();
    output_ctx_phys = (uint64_t)out_phys;
    output_ctx_virt = (void *)VIRT(out_phys);

    void *ep0_phys = pmm_alloc();
    ep0_transfer_ring_phys = (uint64_t)ep0_phys;
    ep0_transfer_ring_virt = (xhci_trb_t *)VIRT(ep0_phys);
    memset(ep0_transfer_ring_virt, 0, PAGE_SIZE);

    void *tr_phys = pmm_alloc();
    ep1_transfer_ring_phys = (uint64_t)tr_phys;
    ep1_transfer_ring_virt = (xhci_trb_t *)VIRT(tr_phys);
    memset(ep1_transfer_ring_virt, 0, PAGE_SIZE);

    void *buf_phys = pmm_alloc();
    hid_report_buffer_phys = (uint64_t)buf_phys;
    hid_report_buffer_virt = (uint8_t *)VIRT(buf_phys);
    memset(hid_report_buffer_virt, 0, PAGE_SIZE);

    g_xhci.op_regs->usbcmd |= 1U;
    while (g_xhci.op_regs->usbsts & 1U) {}

    slot_cmd_state = XHCI_SLOT_STATE_ENABLING;

    xhci_scan_ports();
    xhci_post_command(0, 0, (TRB_TYPE_ENABLE_SLOT << 10));

    serial_puts(COM1, "[xHCI] Exact Offset 0x08 TR Dequeue Pointer Fix Applied.\n");
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