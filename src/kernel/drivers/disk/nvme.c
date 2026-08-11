// nvme.c - Production-grade NVMe Driver implementation with heavy serial diagnostics
#include "nvme.h"
#include "../pci/pci.h"
#include "../serial/serial.h"
#include "../../core/mem/memory.h"
#include "../../core/mem/pmm.h"
#include "../../core/mem/vmm.h"
#include "../../core/gen/io.h"
#include "string.h"
#include "stdio.h"

static nvme_controller_t nvme_ctrl;

// Helper to read 32-bit register from BAR0
static inline uint32_t nvme_read32(nvme_controller_t *ctrl, uint32_t offset) {
    return *((volatile uint32_t *)(ctrl->bar0 + offset));
}

// Helper to write 32-bit register to BAR0
static inline void nvme_write32(nvme_controller_t *ctrl, uint32_t offset, uint32_t value) {
    *((volatile uint32_t *)(ctrl->bar0 + offset)) = value;
}

// Helper to read 64-bit register from BAR0
static inline uint64_t nvme_read64(nvme_controller_t *ctrl, uint32_t offset) {
    return *((volatile uint64_t *)(ctrl->bar0 + offset));
}

// Helper to write 64-bit register to BAR0
static inline void nvme_write64(nvme_controller_t *ctrl, uint32_t offset, uint64_t value) {
    *((volatile uint64_t *)(ctrl->bar0 + offset)) = value;
}

// Find NVMe controller on PCI bus (Class 0x01, Subclass 0x08, ProgIF 0x02)
static int nvme_find_controller(uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func) {
    serial_puts(COM1, "[NVME-DEBUG] Scanning PCI bus topology for NVMe storage...\n");

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_dword((uint8_t)bus, slot, func, 0x00);
                if ((vendor_device & 0xFFFF) == 0xFFFF) continue;

                uint32_t class_reg = pci_read_dword((uint8_t)bus, slot, func, 0x08);
                uint8_t class_code = (class_reg >> 24) & 0xFF;
                uint8_t subclass   = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >> 8)  & 0xFF;

                if (class_code == 0x01 && subclass == 0x08 && prog_if == 0x02) {
                    *out_bus = (uint8_t)bus;
                    *out_slot = slot;
                    *out_func = func;
                    serial_puts(COM1, "[NVME-DEBUG] MATCH FOUND! NVMe Controller located.\n");
                    return 1;
                }
            }
        }
    }
    serial_puts(COM1, "[NVME-DEBUG] WARNING: No NVMe controller found on PCIe.\n");
    return 0;
}

// Force page tables to mark DMA memory as uncacheable (PTE_PCD | PTE_PWT)
static void nvme_make_uncacheable(uint64_t phys_addr, size_t size) {
    uint64_t cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    page_table_t *pml4 = (page_table_t *)VIRT(cr3_val & ~0xFFFULL);

    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        uint64_t page_phys = (phys_addr & ~0xFFFULL) + (i * PAGE_SIZE);
        uint64_t page_virt = VIRT(page_phys);
        // Map with Page-level Cache Disable (PCD) and Write-Through (PWT)
        vmm_map(pml4, page_virt, page_phys, PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
    }
}

// Initialize Submission & Completion Queues with page-aligned physical memory
static int nvme_init_queue_pair(nvme_queue_pair_t *qp, uint16_t sq_size, uint16_t cq_size) {
    
    size_t sq_bytes = sq_size * sizeof(nvme_sq_entry_t);
    size_t cq_bytes = cq_size * sizeof(nvme_cq_entry_t);

    size_t sq_pages = (sq_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t cq_pages = (cq_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    // Allocate continuous physical pages for queues
    void *sq_virt = pmm_alloc_continuous(sq_pages);
    void *cq_virt = pmm_alloc_continuous(cq_pages);

    if (!sq_virt || !cq_virt) {
        serial_puts(COM1, "[NVME-ERROR] Out of physical memory for queue pair allocation!\n");
        return NVME_ERR_NOMEM;
    }

    qp->sq = (nvme_sq_entry_t *)VIRT((uint64_t)sq_virt);
    qp->cq = (nvme_cq_entry_t *)VIRT((uint64_t)cq_virt);
    
    memset(qp->sq, 0, sq_bytes);
    memset(qp->cq, 0, cq_bytes);

    qp->sq_phys = (uint64_t)sq_virt;
    qp->cq_phys = (uint64_t)cq_virt;

    qp->sq_size = sq_size;
    qp->cq_size = cq_size;
    qp->sq_tail = 0;
    qp->cq_head = 0;
    qp->cq_phase = 1;
    nvme_make_uncacheable(qp->sq_phys, sq_bytes);
    nvme_make_uncacheable(qp->cq_phys, cq_bytes);

    serial_puts(COM1, "[NVME-DEBUG] Queue pair allocated successfully.\n");
    return NVME_SUCCESS;
}

// Submit a command to a submission queue and ring the doorbell
static void nvme_submit_command(nvme_controller_t *ctrl, nvme_queue_pair_t *qp, nvme_sq_entry_t *cmd, uint8_t is_admin) {
    uint16_t tail = qp->sq_tail;
    
    // 1. Copy command into submission queue ring buffer
    qp->sq[tail] = *cmd;

    // 2. CRITICAL: Flush CPU cache line to physical RAM so the NVMe controller sees the command via DMA!
    __asm__ volatile("clflush (%0)" : : "r"(&qp->sq[tail]) : "memory");
    __asm__ volatile("mfence" ::: "memory");

    // 3. Advance tail pointer
    qp->sq_tail = (qp->sq_tail + 1) % qp->sq_size;

    // 4. Ring Doorbell (Admin SQ doorbell offset is 0x1000)
    if (is_admin) {
        nvme_write32(ctrl, 0x1000, qp->sq_tail);
    } else {
        nvme_write32(ctrl, 0x1000 + (2 * 4), qp->sq_tail);
    }
}

// Poll completion queue and wait for command ID match
static int nvme_wait_completion(nvme_controller_t *ctrl, nvme_queue_pair_t *qp, uint16_t cid, uint8_t is_admin) {
    uint32_t timeout = 5000000; // 5 seconds timeout loop

    while (timeout--) {
        nvme_cq_entry_t *cqe = &qp->cq[qp->cq_head];
        uint8_t phase = (uint8_t)(cqe->status & 1);

        if (phase == qp->cq_phase) {
            if (cqe->cid == cid) {
                uint16_t status = (uint16_t)((cqe->status >> 1) & 0x7FF);

                qp->cq_head = (qp->cq_head + 1) % qp->cq_size;
                if (qp->cq_head == 0) {
                    qp->cq_phase = !qp->cq_phase;
                }

                // Ring completion queue doorbell
                if (is_admin) {
                    nvme_write32(ctrl, 0x1000 + 4, qp->cq_head);
                } else {
                    nvme_write32(ctrl, 0x1000 + (2 * 4) + 4, qp->cq_head);
                }

                return (status == 0) ? NVME_SUCCESS : NVME_ERR_HARDWARE;
            }
        }
    }
    serial_puts(COM1, "[NVME-ERROR] Command completion timeout reached!\n");
    return NVME_ERR_TIMEOUT;
}

// Send Admin Identify Controller command
static int nvme_identify_controller(nvme_controller_t *ctrl) {
    serial_puts(COM1, "[NVME-DEBUG] Executing Identify Controller command...\n");

    void *buf_virt = pmm_alloc_continuous(1); // 4KB DMA buffer
    if (!buf_virt) return NVME_ERR_NOMEM;
    uint64_t buf_phys = (uint64_t)buf_virt;
    nvme_make_uncacheable(buf_phys, 4096); // Disable caching for DMA buffer
    memset((void *)VIRT(buf_phys), 0, 4096);

    nvme_sq_entry_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.cdw0 = NVME_ADMIN_IDENTIFY | (ctrl->command_id++ << 16);
    cmd.nsid = 0;
    cmd.prp1 = buf_phys;
    cmd.cdw10 = NVME_IDENTIFY_CONTROLLER;

    nvme_submit_command(ctrl, &ctrl->admin_queue, &cmd, 1);
    int err = nvme_wait_completion(ctrl, &ctrl->admin_queue, (uint16_t)(cmd.cdw0 >> 16), 1);

    if (err == NVME_SUCCESS) {
        serial_puts(COM1, "[NVME-DEBUG] Identify Controller response received successfully.\n");
    }

    return err;
}

// Full NVMe Controller Initialization Routine
int nvme_init(void) {
    char buf[32];
    serial_puts(COM1, "[NVME-DEBUG] ========================================\n");
    serial_puts(COM1, "[NVME-DEBUG] STARTING HARDWARE NVMe INITIALIZATION\n");

    uint8_t bus, slot, func;
    if (!nvme_find_controller(&bus, &slot, &func)) {
        serial_puts(COM1, "[NVME-DEBUG] Aborting NVMe init: Controller not present.\n");
        return NVME_ERR_NOTFOUND;
    }

    // Enable PCI Bus Mastering and Memory Space in Command Register
    uint32_t pci_cmd = pci_read_dword(bus, slot, func, 0x04);
    pci_cmd |= (1 << 1) | (1 << 2); // Enable Memory Space & Bus Master
    pci_write_word(bus, slot, func, 0x04, (uint16_t)pci_cmd);
    serial_puts(COM1, "[NVME-DEBUG] PCI Bus Master and Memory Space enabled.\n");

    // Get BAR0 Physical Address
    uint32_t bar0_low = pci_read_dword(bus, slot, func, 0x10);
    uint32_t bar0_high = pci_read_dword(bus, slot, func, 0x14);
    uint64_t bar0_phys = ((uint64_t)bar0_high << 32) | (bar0_low & ~0xF);

    serial_puts(COM1, "[NVME-DEBUG] BAR0 Physical Base: 0x");
    itoa_hex((uint32_t)(bar0_phys >> 32), buf);
    serial_puts(COM1, buf);
    itoa_hex((uint32_t)(bar0_phys & 0xFFFFFFFF), buf);
    serial_puts(COM1, buf);
    serial_puts(COM1, "\n");

    // Map BAR0 using VMM with cache disabled flags (PCD | PWT)
    nvme_ctrl.bar0 = (volatile uint8_t *)pci_map_mmio(bar0_phys, 0x16384); // Map 16KB
    if (!nvme_ctrl.bar0) {
        serial_puts(COM1, "[NVME-ERROR] Failed to map BAR0 MMIO via VMM!\n");
        return NVME_ERR_HARDWARE;
    }
    serial_puts(COM1, "[NVME-DEBUG] BAR0 successfully mapped to virtual memory.\n");
    nvme_write32(&nvme_ctrl, 0x0C, 0xFFFFFFFF);
    serial_puts(COM1, "[NVME-DEBUG] All NVMe interrupts masked (Polling mode active).\n");
    // 1. Disable Controller (CC.EN = 0)
    uint32_t cc = nvme_read32(&nvme_ctrl, NVME_REG_CC);
    cc &= ~NVME_CC_ENABLE;
    nvme_write32(&nvme_ctrl, NVME_REG_CC, cc);
    serial_puts(COM1, "[NVME-DEBUG] Controller disabled (CC.EN = 0). Waiting for CSTS.RDY == 0...\n");

    uint32_t timeout = 5000000;
    while ((nvme_read32(&nvme_ctrl, NVME_REG_CSTS) & NVME_CSTS_RDY) && --timeout);
    if (timeout == 0) {
        serial_puts(COM1, "[NVME-ERROR] Timeout waiting for controller to clear CSTS.RDY!\n");
        return NVME_ERR_TIMEOUT;
    }
    serial_puts(COM1, "[NVME-DEBUG] Controller reset confirmed (CSTS.RDY == 0).\n");

    // 2. Initialize Admin Queues
    if (nvme_init_queue_pair(&nvme_ctrl.admin_queue, NVME_ADMIN_QUEUE_SIZE, NVME_ADMIN_QUEUE_SIZE) != NVME_SUCCESS) {
        return NVME_ERR_NOMEM;
    }

    nvme_write64(&nvme_ctrl, NVME_REG_ASQ, nvme_ctrl.admin_queue.sq_phys);
    nvme_write64(&nvme_ctrl, NVME_REG_ACQ, nvme_ctrl.admin_queue.cq_phys);

    uint32_t aqa = ((NVME_ADMIN_QUEUE_SIZE - 1) << 16) | (NVME_ADMIN_QUEUE_SIZE - 1);
    nvme_write32(&nvme_ctrl, NVME_REG_AQA, aqa);
    serial_puts(COM1, "[NVME-DEBUG] Admin queues configured in hardware registers.\n");

    // 3. Enable Controller (CC.EN = 1)
    cc = NVME_CC_ENABLE | NVME_CC_CSS_NVM | NVME_CC_AMS_RR | NVME_CC_SHN_NONE | NVME_CC_IOSQES | NVME_CC_IOCQES;
    nvme_write32(&nvme_ctrl, NVME_REG_CC, cc);
    serial_puts(COM1, "[NVME-DEBUG] Controller enabled (CC.EN = 1). Waiting for CSTS.RDY == 1...\n");

    timeout = 5000000;
    while (!(nvme_read32(&nvme_ctrl, NVME_REG_CSTS) & NVME_CSTS_RDY) && --timeout);
    if (timeout == 0) {
        serial_puts(COM1, "[NVME-ERROR] Timeout waiting for controller readiness (CSTS.RDY == 1)!\n");
        return NVME_ERR_TIMEOUT;
    }
    serial_puts(COM1, "[NVME-DEBUG] SUCCESS: NVMe Controller is Ready and Operational!\n");

    nvme_ctrl.command_id = 0;

    // 4. Send Identify Controller command
    if (nvme_identify_controller(&nvme_ctrl) != NVME_SUCCESS) {
        serial_puts(COM1, "[NVME-ERROR] Failed to identify NVMe controller parameters.\n");
        return NVME_ERR_HARDWARE;
    }

    serial_puts(COM1, "[NVME-DEBUG] NVMe Driver initialization sequence fully completed.\n");
    serial_puts(COM1, "[NVME-DEBUG] ========================================\n");
    return NVME_SUCCESS;
}