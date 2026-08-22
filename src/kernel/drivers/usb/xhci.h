// src/kernel/drivers/usb/xhci.h - Extended xHCI Specification with Doorbells & Contexts
#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include <stdbool.h>
#include "usb.h"

#define XHCI_TRB_RING_SIZE 64

// TRB Types
#define TRB_TYPE_NORMAL           1
#define TRB_TYPE_SETUP_STAGE      2
#define TRB_TYPE_DATA_STAGE       3
#define TRB_TYPE_STATUS_STAGE     4
#define TRB_TYPE_LINK             6
#define TRB_TYPE_ENABLE_SLOT      9
#define TRB_TYPE_DISABLE_SLOT     10
#define TRB_TYPE_ADDRESS_DEVICE   11
#define TRB_TYPE_CONFIG_ENDPOINT  12
#define TRB_TYPE_TRANSFER_EVENT   32
#define TRB_TYPE_CMD_COMPLETION   33
#define TRB_TYPE_PORT_STATUS_CHG  34

// PORTSC Register Bits
#define PORTSC_CCS        (1U << 0)  // Current Connect Status
#define PORTSC_PED        (1U << 1)  // Port Enabled/Disabled
#define PORTSC_OCA        (1U << 3)  // Over-Current Active
#define PORTSC_PR         (1U << 4)  // Port Reset
#define PORTSC_PLS_MASK   (0x0FU << 5)
#define PORTSC_PP         (1U << 9)  // Port Power
#define PORTSC_SPEED_MASK (0x0FU << 10)
#define PORTSC_CSC        (1U << 17) // Connect Status Change
#define PORTSC_PRC        (1U << 21) // Port Reset Change

// xHCI Transfer Request Block (16 Bytes)
typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

// Event Ring Segment Table Entry (16 Bytes)
typedef struct {
    uint64_t ring_segment_base;
    uint16_t ring_segment_size;
    uint16_t reserved0;
    uint32_t reserved1;
} __attribute__((packed)) xhci_erst_entry_t;

// Port Registers Structure
typedef struct {
    uint32_t portsc;
    uint32_t portpmsc;
    uint32_t portli;
    uint32_t portlpm;
} __attribute__((packed)) xhci_port_regs_t;

// Capability Registers (Read-Only)
typedef struct {
    uint8_t  caplength;
    uint8_t  reserved;
    uint16_t hciversion;
    uint32_t hcsparams1;
    uint32_t hcsparams2;
    uint32_t hcsparams3;
    uint32_t hccparams1;
    uint32_t dboff;
    uint32_t rtoff;
    uint32_t hccparams2;
} __attribute__((packed)) xhci_cap_regs_t;

// Operational Registers
typedef struct {
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t pagesize;
    uint32_t reserved0[2];
    uint32_t dnctrl;
    uint64_t crcr;
    uint32_t reserved1[4];
    uint64_t dcbaap;
    uint32_t config;
} __attribute__((packed)) xhci_op_regs_t;

// Interrupter Register Set (Runtime Registers)
typedef struct {
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint32_t reserved;
    uint64_t erstba;
    uint64_t erdp;
} __attribute__((packed)) xhci_interrupter_regs_t;

typedef struct {
    uint32_t mfindex;
    uint8_t  reserved[0x1C];
    xhci_interrupter_regs_t ir[1];
} __attribute__((packed)) xhci_rt_regs_t;

// Slot Context (32 Bytes)
typedef struct {
    uint32_t info1;
    uint32_t info2;
    uint32_t tt_info;
    uint32_t state;
    uint32_t reserved[4];
} __attribute__((packed)) xhci_slot_ctx_t;

// Endpoint Context (32 Bytes)
typedef struct {
    uint32_t info1;
    uint32_t info2;
    uint64_t tr_dequeue_ptr;
    uint32_t avg_trb_len;
    uint32_t reserved[3];
} __attribute__((packed)) xhci_ep_ctx_t;

// Controller State
typedef struct {
    volatile xhci_cap_regs_t  *cap_regs;
    volatile xhci_op_regs_t   *op_regs;
    volatile xhci_rt_regs_t   *rt_regs;
    volatile uint32_t         *db_regs;
    volatile xhci_port_regs_t *port_regs;

    uint32_t max_slots;
    uint32_t max_ports;

    // Device Context Base Address Array (DCBAA)
    uint64_t *dcbaa_virt;
    uint64_t  dcbaa_phys;

    // Command Ring
    xhci_trb_t *cmd_ring_virt;
    uint64_t    cmd_ring_phys;
    uint32_t    cmd_enqueue_idx;
    uint8_t     cmd_pcs; // Producer Cycle State

    // Event Ring
    xhci_trb_t        *event_ring_virt;
    uint64_t           event_ring_phys;
    xhci_erst_entry_t *erst_virt;
    uint64_t           erst_phys;
    uint32_t           event_dequeue_idx;
    uint8_t            event_ccs; // Consumer Cycle State
} xhci_t;

void xhci_ring_doorbell(uint32_t slot, uint32_t target);
void xhci_poll_event_ring(void);
void xhci_scan_ports(void);

#endif // XHCI_H