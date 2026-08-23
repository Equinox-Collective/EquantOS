// src/kernel/drivers/usb/xhci.c - Multi-Slot xHCI Driver with Exact Event Transfer Length
#include "xhci.h"
#include "usb_hid.h"
#include "../pci/pci.h"
#include "../serial/serial.h"
#include "../../core/initcall.h"
#include "../../core/mem/pmm.h"
#include "../../core/mem/vmm.h"
#include "string.h"

#define XHCI_MAX_SLOTS_SUPPORTED 8

typedef enum {
    XHCI_SLOT_STATE_DISABLED      = 0,
    XHCI_SLOT_STATE_ENABLING      = 1,
    XHCI_SLOT_STATE_ADDRESSING    = 2,
    XHCI_SLOT_STATE_CONFIGURING   = 3,
    XHCI_SLOT_STATE_ADDRESSED     = 4
} xhci_slot_state_t;

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

typedef struct {
    uint32_t          slot_id;
    uint32_t          port_num;
    uint32_t          port_speed;
    xhci_slot_state_t state;

    xhci_input_ctx_t *input_ctx_virt;
    uint64_t          input_ctx_phys;
    void             *output_ctx_virt;
    uint64_t          output_ctx_phys;

    xhci_trb_t       *ep0_tr_virt;
    uint64_t          ep0_tr_phys;

    xhci_trb_t       *ep_tr_virt;
    uint64_t          ep_tr_phys;
    uint32_t          ep_enqueue_idx;
    uint8_t           ep_pcs;

    uint8_t          *report_buf_virt;
    uint64_t          report_buf_phys;
} xhci_slot_device_t;

static xhci_t g_xhci;
static xhci_slot_device_t g_slots[XHCI_MAX_SLOTS_SUPPORTED];

void xhci_ring_doorbell(uint32_t slot, uint32_t target) {
    if (!g_xhci.db_regs) return;
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

static void xhci_alloc_slot_resources(xhci_slot_device_t *slot) {
    if (slot->input_ctx_virt) return;

    void *in_phys = pmm_alloc();
    slot->input_ctx_phys = (uint64_t)in_phys;
    slot->input_ctx_virt = (xhci_input_ctx_t *)VIRT(in_phys);

    void *out_phys = pmm_alloc();
    slot->output_ctx_phys = (uint64_t)out_phys;
    slot->output_ctx_virt = (void *)VIRT(out_phys);

    void *ep0_phys = pmm_alloc();
    slot->ep0_tr_phys = (uint64_t)ep0_phys;
    slot->ep0_tr_virt = (xhci_trb_t *)VIRT(ep0_phys);
    memset(slot->ep0_tr_virt, 0, PAGE_SIZE);

    void *tr_phys = pmm_alloc();
    slot->ep_tr_phys = (uint64_t)tr_phys;
    slot->ep_tr_virt = (xhci_trb_t *)VIRT(tr_phys);
    memset(slot->ep_tr_virt, 0, PAGE_SIZE);

    void *buf_phys = pmm_alloc();
    slot->report_buf_phys = (uint64_t)buf_phys;
    slot->report_buf_virt = (uint8_t *)VIRT(buf_phys);
    memset(slot->report_buf_virt, 0, PAGE_SIZE);

    slot->ep_enqueue_idx = 0;
    slot->ep_pcs = 1;
}

static void xhci_arm_hid_endpoint(xhci_slot_device_t *slot) {
    if (!slot || !slot->ep_tr_virt || !slot->report_buf_virt) return;

    xhci_trb_t *trb = &slot->ep_tr_virt[slot->ep_enqueue_idx];
    trb->parameter = slot->report_buf_phys;
    trb->status    = 8;
    trb->control   = (TRB_TYPE_NORMAL << 10) | (1U << 5) | (1U << 2) | slot->ep_pcs;

    slot->ep_enqueue_idx++;
    if (slot->ep_enqueue_idx >= 15) {
        xhci_trb_t *link = &slot->ep_tr_virt[slot->ep_enqueue_idx];
        link->parameter = slot->ep_tr_phys;
        link->status    = 0;
        link->control   = (TRB_TYPE_LINK << 10) | (1U << 1) | slot->ep_pcs;
        
        slot->ep_enqueue_idx = 0;
        slot->ep_pcs ^= 1;
    }

    xhci_ring_doorbell(slot->slot_id, 3);
}

static void xhci_set_configuration(xhci_slot_device_t *slot) {
    if (!slot || !slot->ep0_tr_virt) return;

    uint64_t setup_pkt = 0x0000000000010900ULL; // SET_CONFIGURATION(1)

    slot->ep0_tr_virt[0].parameter = setup_pkt;
    slot->ep0_tr_virt[0].status    = 8;
    slot->ep0_tr_virt[0].control   = (TRB_TYPE_SETUP_STAGE << 10) | (1U << 6) | 1U;

    slot->ep0_tr_virt[1].parameter = 0;
    slot->ep0_tr_virt[1].status    = 0;
    slot->ep0_tr_virt[1].control   = (TRB_TYPE_STATUS_STAGE << 10) | (1U << 5) | (1U << 16) | 1U;

    xhci_ring_doorbell(slot->slot_id, 1);
}

static void xhci_address_device(xhci_slot_device_t *slot) {
    if (!slot) return;

    xhci_alloc_slot_resources(slot);

    memset(slot->input_ctx_virt, 0, sizeof(xhci_input_ctx_t));
    memset(slot->output_ctx_virt, 0, PAGE_SIZE);

    g_xhci.dcbaa_virt[slot->slot_id] = slot->output_ctx_phys;

    slot->input_ctx_virt->ctrl.add_flags = 3U; // A0 + A1
    slot->input_ctx_virt->slot.info1 = (1U << 27) | ((slot->port_speed & 0x0F) << 20);
    slot->input_ctx_virt->slot.info2 = ((slot->port_num & 0xFF) << 16);

    uint32_t max_packet_size = (slot->port_speed == 2 || slot->port_speed == 1) ? 8 : 64;
    slot->input_ctx_virt->ep0.info0 = 0;
    slot->input_ctx_virt->ep0.info1 = (3U << 1) | (4U << 3) | (max_packet_size << 16);
    slot->input_ctx_virt->ep0.tr_dequeue_ptr = slot->ep0_tr_phys | 1U;
    slot->input_ctx_virt->ep0.avg_trb_len = 8;

    xhci_post_command(slot->input_ctx_phys, 0, (TRB_TYPE_ADDRESS_DEVICE << 10) | (slot->slot_id << 24));
}

static void xhci_configure_hid_endpoint(xhci_slot_device_t *slot) {
    if (!slot) return;

    memset(slot->input_ctx_virt, 0, sizeof(xhci_input_ctx_t));

    slot->input_ctx_virt->ctrl.add_flags = (1U << 0) | (1U << 3); // A0 + A3
    slot->input_ctx_virt->slot.info1 = (3U << 27) | ((slot->port_speed & 0x0F) << 20);
    slot->input_ctx_virt->slot.info2 = ((slot->port_num & 0xFF) << 16);

    slot->input_ctx_virt->ep1_in.info0 = (3U << 16); // Interval = 3 (8ms)
    slot->input_ctx_virt->ep1_in.info1 = (3U << 1) | (7U << 3) | (8U << 16); // Interrupt IN, MaxPktSize=8
    slot->input_ctx_virt->ep1_in.tr_dequeue_ptr = slot->ep_tr_phys | 1U;
    slot->input_ctx_virt->ep1_in.avg_trb_len = 8;

    xhci_post_command(slot->input_ctx_phys, 0, (TRB_TYPE_CONFIG_ENDPOINT << 10) | (slot->slot_id << 24));
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

        if (trb_type == TRB_TYPE_CMD_COMPLETION) {
            if (slot_id < XHCI_MAX_SLOTS_SUPPORTED) {
                xhci_slot_device_t *slot = &g_slots[slot_id];

                if (slot->state == XHCI_SLOT_STATE_ENABLING && completion_code == 1) {
                    slot->state = XHCI_SLOT_STATE_ADDRESSING;
                    xhci_address_device(slot);
                } 
                else if (slot->state == XHCI_SLOT_STATE_ADDRESSING && completion_code == 1) {
                    slot->state = XHCI_SLOT_STATE_CONFIGURING;
                    xhci_configure_hid_endpoint(slot);
                }
                else if (slot->state == XHCI_SLOT_STATE_CONFIGURING && completion_code == 1) {
                    slot->state = XHCI_SLOT_STATE_ADDRESSED;
                    xhci_set_configuration(slot);
                    xhci_arm_hid_endpoint(slot);
                }
            }
        } 
        else if (trb_type == TRB_TYPE_TRANSFER_EVENT) {
            if (slot_id < XHCI_MAX_SLOTS_SUPPORTED) {
                xhci_slot_device_t *slot = &g_slots[slot_id];

                uint32_t residual = event->status & 0x00FFFFFF;
                size_t bytes_transferred = (residual < 8) ? (8 - residual) : 8;
                if (bytes_transferred == 0) bytes_transferred = 8;

                usb_hid_parse_report(slot->report_buf_virt, bytes_transferred);
                xhci_arm_hid_endpoint(slot);
            }
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
            uint32_t port_num = i + 1;
            uint32_t port_speed = (portsc >> 10) & 0x0F;

            for (uint32_t s = 1; s < XHCI_MAX_SLOTS_SUPPORTED; s++) {
                if (g_slots[s].state == XHCI_SLOT_STATE_DISABLED) {
                    g_slots[s].slot_id = s;
                    g_slots[s].port_num = port_num;
                    g_slots[s].port_speed = port_speed;
                    g_slots[s].state = XHCI_SLOT_STATE_ENABLING;

                    g_xhci.port_regs[i].portsc = (portsc & ~PORTSC_PED) | PORTSC_PR;
                    while (g_xhci.port_regs[i].portsc & PORTSC_PR) {}

                    xhci_post_command(0, 0, (TRB_TYPE_ENABLE_SLOT << 10));
                    break;
                }
            }
        }
    }
}

static int xhci_probe(pci_device_t *dev) {
    memset(g_slots, 0, sizeof(g_slots));

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

    g_xhci.op_regs->usbcmd |= 1U;
    while (g_xhci.op_regs->usbsts & 1U) {}

    xhci_scan_ports();

    serial_puts(COM1, "[xHCI] Controller Initialized (Multi-Slot Support Ready).\n");
    return 0;
}

static pci_device_id_t xhci_pci_ids[] = {
    { 0xFFFF, 0xFFFF, 0x0C, 0x03 },
    { 0, 0, 0, 0 }
};

static pci_driver_t xhci_pci_driver = {
    .name     = "xHCI USB Host Controller",
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